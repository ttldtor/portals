// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/style_static.hpp>

using namespace portals;
namespace sc = style::ct;

#if defined(STATIC_STYLE_INVALID_RGB)
constexpr auto INVALID = sc::rgb(256, 0, 0);
#elif defined(STATIC_STYLE_INVALID_MIX)
constexpr auto INVALID = sc::mix(sc::rgb(0, 0, 0), sc::rgb(255, 255, 255), -1);
#elif defined(STATIC_STYLE_INVALID_STATE)
constexpr auto INVALID = sc::selector(static_cast<StyleState>(100));
#elif defined(STATIC_STYLE_INVALID_METRIC)
constexpr auto INVALID = sc::sheet(sc::parameters(), sc::rule(sc::selector(), sc::minWidth(std::int32_t{-1})));
#elif defined(STATIC_STYLE_INVALID_STATE_METRIC)
constexpr auto INVALID =
  sc::sheet(sc::parameters(), sc::rule(sc::selector(StyleState::FOCUSED), sc::minWidth(std::int32_t{1})));
#elif defined(STATIC_STYLE_INVALID_PARAMETERS)
constexpr auto INVALID = sc::parameters(sc::parameter<"duplicate">(true), sc::parameter<"duplicate">(false));
#elif defined(STATIC_STYLE_INVALID_EMPTY_PARAMETER)
constexpr auto INVALID = sc::parameters(sc::parameter<"">(true));
#elif defined(STATIC_STYLE_INVALID_PARAMETER_TYPE)
constexpr auto INVALID = sc::parameter<"wrong">(1.5);
#elif defined(STATIC_STYLE_INVALID_RUNTIME_VALUE)
void invalid(std::int32_t runtime) {
  const auto value = sc::parameter<"value">(runtime);
}
#elif defined(STATIC_STYLE_INVALID_RULE_LIMIT)
template <std::size_t... Index> consteval auto tooMany(std::index_sequence<Index...>) {
  return sc::sheet(sc::parameters(), (static_cast<void>(Index), sc::rule(sc::selector()))...);
}

constexpr auto INVALID = tooMany(std::make_index_sequence<MAX_STYLE_RULES + 1>{});
#elif defined(STATIC_STYLE_INVALID_TOKEN)
constexpr auto INVALID = sc::sheet(sc::parameters(), sc::rule(sc::selector(), sc::bold(sc::token<"unknown">())));
#elif defined(STATIC_STYLE_INVALID_DECLARATION_TYPE)
constexpr auto INVALID = sc::sheet(sc::parameters(sc::parameter<"wrong">(true)),
                                   sc::rule(sc::selector(), sc::foreground(sc::token<"wrong">())));
#elif defined(STATIC_STYLE_INVALID_MIX_TYPE)
constexpr auto INVALID =
  sc::sheet(sc::parameters(), sc::rule(sc::selector(), sc::foreground(sc::mix(true, false, 50))));
#elif defined(STATIC_STYLE_INVALID_DECLARATION_DUPLICATE)
constexpr auto INVALID = sc::rule(sc::selector(), sc::bold(true), sc::bold(false));
#else
constexpr auto DESCRIPTION =
  sc::sheet(sc::parameters(sc::parameter<"value">(true)), sc::rule(sc::selector(), sc::bold(sc::token<"value">())));

void invalid(sc::LiveTheme<decltype(DESCRIPTION)> &handle) {
#  if defined(STATIC_STYLE_INVALID_UPDATE_UNKNOWN)
  (void)handle.set<"unknown">(true);
#  elif defined(STATIC_STYLE_INVALID_UPDATE_TYPE)
  (void)handle.set<"value">(1);
#  elif defined(STATIC_STYLE_INVALID_UPDATE_DUPLICATE)
  (void)handle.apply(sc::change<"value">(true), sc::change<"value">(false));
#  endif
}
#endif
