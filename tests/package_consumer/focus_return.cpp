// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <cstdlib>

namespace {
template <class T, class E> T take(std::expected<T, E> result) {
  if (!result) {
    std::exit(EXIT_FAILURE);
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

void check(bool value) {
  if (!value) {
    std::exit(EXIT_FAILURE);
  }
}
} // namespace

int main() {
  using namespace portals;
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<Column> panel;
  ui::Ref<Button> outside, accept, open, back;

  take(ui::mount(*scene, ui::column(ui::button(outside, "Outside",
                                               [] {
                                               }),
                                    ui::column(panel, ui::button(accept, "Accept",
                                                                 [] {
                                                                 })
                                                        .focusDefault())
                                      .focusScope(),
                                    ui::button(open, "Open",
                                               [panel] {
                                                 (void)panel.enterFocusScope();
                                               }),
                                    ui::button(back, "Back", [panel] {
                                      (void)panel.leaveFocusScope();
                                    }))));
  take(outside.focus());
  take(app->router().invoke(open.node(), {Action::ACTIVATE}));
  check(take(app->router().focused()) == accept.node());
  take(app->router().invoke(back.node(), {Action::ACTIVATE}));
  check(take(app->router().focused()) == outside.node());
  take(panel.enterFocusScope());
  take(app->tree().setVisible(panel.node(), false));
  check(take(app->router().focused()) == outside.node());
  check(take(scene->diagnostics()).errors.empty());
  return 0;
}
