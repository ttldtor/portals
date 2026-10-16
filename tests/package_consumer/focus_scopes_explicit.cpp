// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <cstdlib>
#include <iostream>

namespace {
template <class T, class E> T take(std::expected<T, E> result) {
  if (!result) {
    std::cerr << "focus scope consumer operation failed\n";
    std::exit(EXIT_FAILURE);
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

void check(bool value) {
  if (!value) {
    std::cerr << "focus scope consumer selected the wrong target\n";
    std::exit(EXIT_FAILURE);
  }
}
} // namespace

int main() {
  using namespace portals;
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<Column> group;
  ui::Ref<Button> first, preferred, outside;

  take(ui::mount(*scene, ui::column(ui::column(group,
                                               ui::button(first, "First",
                                                          [] {
                                                          }),
                                               ui::button(preferred, "Preferred",
                                                          [] {
                                                          })),
                                    ui::button(outside, "Outside", [] {
                                    }))));
  take(scene->focusScope(group.node()));
  take(scene->focusDefault(preferred.node()));
  take(scene->focus(group.node()));
  check(take(app->router().focused()) == preferred.node());
  take(scene->focus(first.node()));
  take(scene->focus(outside.node()));
  take(scene->focus(group.node()));
  check(take(app->router().focused()) == first.node());
  check(take(app->router().traverse()) == preferred.node());
  check(take(app->router().traverse()) == outside.node());
  take(scene->remove(group.node()));

  auto stale = group.focus();

  check(!stale && std::get<TreeError>(stale.error().cause) == TreeError::STALE_NODE);
  scene.reset();

  auto closed = group.focus();

  check(!closed && std::get<TreeError>(closed.error().cause) == TreeError::CLOSED);

  return 0;
}
