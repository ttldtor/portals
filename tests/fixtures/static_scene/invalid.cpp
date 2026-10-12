// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_static.hpp>

#include <ctime>

using namespace portals;

#if defined(STATIC_INVALID_DUPLICATE_ID)
constexpr auto SCENE =
  ui::ct::scene(ui::ct::column(ui::ct::label<"same">("First"), ui::ct::row(ui::ct::button<"same">("Second"))));
#elif defined(STATIC_INVALID_SIZE)
constexpr auto NODE = ui::ct::label("Invalid", {}, {.width = {.minimum = 4, .maximum = 2}});
#elif defined(STATIC_INVALID_LAYOUT)
constexpr auto NODE = ui::ct::column(LinearOptions{.gap = -1}, ui::ct::label("Invalid"));
#elif defined(STATIC_INVALID_MODE)
constexpr auto NODE = ui::ct::lineEdit("Invalid", static_cast<EditMode>(-1));
#else
constexpr auto SCENE = ui::ct::scene(ui::ct::column(
  ui::ct::label<"label">("Title"), ui::ct::lineEdit<"field">("Initial"), ui::ct::button<"button">("Apply")));

void invalidRuntimeAdapter() {
#  if defined(STATIC_INVALID_UNKNOWN_ID)
  auto recipe = ui::ct::describe(SCENE, ui::ct::reference<"missing">(ui::Ref<Label>{}));
#  elif defined(STATIC_INVALID_REFERENCE_TYPE)
  auto recipe = ui::ct::describe(SCENE, ui::ct::reference<"field">(ui::Ref<Button>{}));
#  elif defined(STATIC_INVALID_CLICK_TYPE)
  auto recipe = ui::ct::describe(SCENE, ui::ct::onClick<"label">([] {
                                 }));
#  elif defined(STATIC_INVALID_BIND_TYPE)
  auto recipe = ui::ct::describe(SCENE, ui::ct::bind<"button">(ManagedState<std::string>{}));
#  elif defined(STATIC_INVALID_DUPLICATE_ADAPTER)
  auto recipe = ui::ct::describe(SCENE, ui::ct::onClick<"button">([] {
                                 }),
                                 ui::ct::onClick<"button">([] {
                                 }));
#  elif defined(STATIC_INVALID_RUNTIME_TEXT)
  char value[] = "Runtime";
  value[0] = static_cast<char>(std::time(nullptr));
  auto node = ui::ct::label(value);
#  else
#    error A specific invalid static scene fixture must be selected
#  endif
}
#endif
