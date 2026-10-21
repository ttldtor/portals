// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
// Compile and run with both PORTALS_COMPOSITE_WINDOWS_FIRST and PORTALS_COMPOSITE_WIDE set to 0/1.
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#if PORTALS_COMPOSITE_WIDE
#  ifndef UNICODE
#    define UNICODE
#  endif
#  ifndef _UNICODE
#    define _UNICODE
#  endif
#else
#  undef UNICODE
#  undef _UNICODE
#endif

#define PORTALS_STRINGIFY_DETAIL(value) #value
#define PORTALS_STRINGIFY(value) PORTALS_STRINGIFY_DETAIL(value)

#if PORTALS_COMPOSITE_WIDE
constexpr char UNICODE_MACRO_BEFORE[] = PORTALS_STRINGIFY(UNICODE portalsUnicodeMacroEnd);
#endif

#if PORTALS_COMPOSITE_WINDOWS_FIRST
#  include <windows.h>
constexpr char NATIVE_MACRO_BEFORE[] = PORTALS_STRINGIFY(MessageBox);
#endif

#include <portals/message_box.hpp>
#include <portals/ui.hpp>

#if PORTALS_COMPOSITE_WINDOWS_FIRST
static_assert(std::string_view(NATIVE_MACRO_BEFORE) == PORTALS_STRINGIFY(MessageBox));
#else
#  ifdef MessageBox
#    error MessageBox macro leaked from the Portals headers
#  endif
#  include <windows.h>
#endif

using namespace portals;
using namespace portals::ui;

static_assert(std::same_as<MessageDialog, ui::MessageBox>);
static_assert(std::same_as<ui::Ref<MessageDialog>, ui::Ref<ui::MessageBox>>);

#if PORTALS_COMPOSITE_WIDE
static_assert(std::string_view(UNICODE_MACRO_BEFORE) == PORTALS_STRINGIFY(UNICODE portalsUnicodeMacroEnd));
static_assert(std::string_view(PORTALS_STRINGIFY(MessageBox)) == "MessageBoxW");
#else
#  ifdef UNICODE
#    error UNICODE macro leaked from the Portals headers
#  endif
static_assert(std::string_view(PORTALS_STRINGIFY(MessageBox)) == "MessageBoxA");
#endif

int main() {
  ui::MessageBox legacy;
  ui::Ref<MessageDialog> canonical(legacy);
  const auto result = canonical.cancel().result();
  const auto *cause = !result ? std::get_if<TreeError>(&result.error().cause) : nullptr;
  auto native = &::MessageBox;

  (void)native;

  return cause && *cause == TreeError::CLOSED ? 0 : 1;
}
