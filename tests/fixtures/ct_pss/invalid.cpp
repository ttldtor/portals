// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/style_pss.hpp>

#include <array>
#include <string_view>
#include <utility>

using namespace portals;
using namespace std::literals;
namespace sc = style::ct;

namespace {
constexpr std::size_t LARGE_PREFIX_RULES = 200;
constexpr std::size_t OVER_LIMIT_RULES = 257;
constexpr std::string_view PREFIX_RULE = "*{}\n";

#if defined(PSS_INVALID_LARGE)
constexpr std::size_t PREFIX_COUNT = LARGE_PREFIX_RULES;
#else
constexpr std::size_t PREFIX_COUNT = 0;
#endif

#if defined(PSS_INVALID_SELECTOR)
constexpr std::string_view FRAGMENT = "? {}"sv;
#elif defined(PSS_INVALID_PROPERTY)
constexpr std::string_view FRAGMENT = "* { missing: true; }"sv;
#elif defined(PSS_INVALID_METRIC)
constexpr std::string_view FRAGMENT = "* { min-width: -1; }"sv;
#elif defined(PSS_INVALID_UTF8)
constexpr std::string_view FRAGMENT = "/*\xff*/ *{}"sv;
#elif defined(PSS_INVALID_NUL)
constexpr std::string_view FRAGMENT = "/*\0*/ *{}"sv;
#else
constexpr std::string_view FRAGMENT = ""sv;
#endif

// The source is data, not a string NTTP: a failing template must not print all rules.
template <std::size_t Count> consteval auto source() {
  std::array<char, Count * PREFIX_RULE.size() + FRAGMENT.size()> result{};
  std::size_t offset{};

  for (std::size_t rule = 0; rule < Count; ++rule) {
    for (const char byte : PREFIX_RULE) {
      result[offset++] = byte;
    }
  }

  for (const char byte : FRAGMENT) {
    result[offset++] = byte;
  }

  return result;
}

#if defined(PSS_INVALID_RULE_LIMIT)
constexpr auto SOURCE = source<OVER_LIMIT_RULES>();
#else
constexpr auto SOURCE = source<PREFIX_COUNT>();
#endif
} // namespace

#if defined(PSS_INVALID_TOKEN) || defined(PSS_INVALID_TYPE)
consteval auto typedSource() {
  const auto normal = sc::flatRule(sc::all(), sc::bold(true));
  auto rules = [&]<std::size_t... Index>(std::index_sequence<Index...>) {
    return std::array{(static_cast<void>(Index), normal)...};
  }(std::make_index_sequence<PREFIX_COUNT + 1>{});

#  if defined(PSS_INVALID_TOKEN)
  rules.back() = sc::flatRule(sc::parameters(), sc::id<"action">(), sc::fg(sc::token<"missing">()));
#  else
  rules.back() =
    sc::flatRule(sc::parameters(sc::parameter<"wrong">(true)), sc::id<"action">(), sc::fg(sc::token<"wrong">()));
#  endif

  return sc::compactSheet(rules);
}

constexpr auto INVALID = typedSource();
#elif defined(PSS_INVALID_POINTER_PROVIDER)
constexpr auto INVALID = sc::pss([] {
  return "/*\0*/ *{}";
});
#elif defined(PSS_INVALID_DIRECT_NUL)
constexpr auto INVALID = sc::pss("/*\0*/ *{}");
#else
constexpr auto INVALID = sc::pss([] {
  return std::string_view(SOURCE.data(), SOURCE.size());
});
#endif
