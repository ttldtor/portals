// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/managed_text_fixture.hpp"

namespace {
void unicodeAndOwnership() {
  auto original =
    take(GraphemeText::from("one\xc2\xa0two can't e\xcc\x81 \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb"));
  auto tokens = take(words(original));
  check(tokens.size() == 5, "whitespace words preserve punctuation, combining and emoji");
  auto retained = take(tokens.at(4));
  original = take(GraphemeText::from("replaced"));
  check(retained.bytes() == "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb", "segment retains original bytes");
  auto a = take(words(take(GraphemeText::from("a  b"))));
  auto b = take(words(take(GraphemeText::from(" a\tb "))));
  check(a == b, "word equality ignores delimiter offsets");
  const std::string_view separators[]{"\x09",         "\x0a",         "\x0b",         "\x0c",         "\x0d",
                                      "\x20",         "\xc2\x85",     "\xc2\xa0",     "\xe1\x9a\x80", "\xe2\x80\x80",
                                      "\xe2\x80\x81", "\xe2\x80\x82", "\xe2\x80\x83", "\xe2\x80\x84", "\xe2\x80\x85",
                                      "\xe2\x80\x86", "\xe2\x80\x87", "\xe2\x80\x88", "\xe2\x80\x89", "\xe2\x80\x8a",
                                      "\xe2\x80\xa8", "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80"};
  for (auto separator : separators) {
    auto input = take(GraphemeText::from(std::string("a") + std::string(separator) + "b"));
    check(take(words(input)).size() == 2, "all explicit White_Space scalars delimit words");
  }
  check(take(words(take(GraphemeText::from("a\xe2\x80\x8b"
                                           "b\xef\xbb\xbf"
                                           "c"))))
            .size() == 1,
        "zero-width space and BOM are not White_Space delimiters");
  auto markedSpace = take(words(take(GraphemeText::from("a \xcc\x81"
                                                        "b"))));
  check(markedSpace.size() == 1, "space with nonwhitespace combining scalar is not a delimiter");
  auto clusters = take(graphemes(take(GraphemeText::from("e\xcc\x81 \xf0\x9f\x87\xba\xf0\x9f\x87\xb8"))));
  check(clusters.size() == 3, "graphemes include whitespace and whole regional indicator pair");
  check(take(words(take(GraphemeText::from("")))).size() == 0, "empty input has zero tokens");
  check(!tokens.at(tokens.size()), "token access bounds are checked");
  WidthPolicy alternate;
  ++alternate.revision;
  check(!(take(GraphemeText::from("a")) == take(GraphemeText::from("a", alternate))),
        "full policy participates in equality");
}

void limitsAndAllocation() {
  auto text = take(GraphemeText::from("one two three"));
  auto capped = words(text, TextSplitOptions{.maxTokens = 2});
  check(!capped && capped.error() == TreeError::NODE_LIMIT, "token cap rejects whole collection");
  check(!words(text, TextSplitOptions{.maxTokens = 0}), "zero token cap rejected");
  TextSplitOptions limited;
  limited.limits.max_input_bytes = 3;
  check(!words(text, limited), "whole input limits apply before segmentation");
  const auto before = allocationTest::count;
  auto baseline = take(words(text));
  const auto allocations = allocationTest::count - before;
  for (std::size_t i = 0; i < allocations; ++i) {
    allocationTest::remaining = static_cast<std::ptrdiff_t>(i);
    auto failed = words(text);
    allocationTest::remaining = -1;
    check(!failed && failed.error() == TreeError::NO_MEMORY, "split allocation is checked and atomic");
  }
  check(baseline.size() == 3, "retained collection survives failed candidates");
  std::cout << "split allocation points: " << allocations << '\n';
}
} // namespace

int main() {
  try {
    unicodeAndOwnership();
    limitsAndAllocation();
  } catch (const std::exception &error) {
    allocationTest::remaining = -1;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
