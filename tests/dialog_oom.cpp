// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/managed_text_fixture.hpp"

namespace {
constexpr std::size_t MAX_FAILURE_POINTS = 4096;

auto recipe(ui::Ref<OverlayHost> host, ui::Ref<Button> opener, ui::Ref<Dialog> first, ui::Ref<Button> inside,
            ui::Ref<Dialog> second) {
  return ui::overlayHost(host, ui::button("Background").reference(opener).onClick([] {
  }),
                         ui::dialog(first, "First", ui::button("Accept").reference(inside).onClick([] {
                         })),
                         ui::dialog(second, "Second", ui::label("Retained")));
}

void mountRollback() {
  std::size_t failures{};
  for (std::size_t point = 0; point != MAX_FAILURE_POINTS; ++point) {
    Fixture f;
    ui::Ref<OverlayHost> host;
    ui::Ref<Button> opener, inside;
    ui::Ref<Dialog> first, second;
    auto description = recipe(host, opener, first, inside, second);
    allocationTest::remaining = static_cast<std::ptrdiff_t>(point);
    auto result = ui::mount(*f.scene, std::move(description));
    allocationTest::remaining = -1;
    if (result) {
      check(host.node() != Node<OverlayHost>{} && first.node() != Node<Dialog>{}, "complete cold publication");
      std::cout << "mount allocation failures=" << failures << "; success index=" << point << '\n';
      return;
    }
    ++failures;
    check(treeCause(result.error(), TreeError::NO_MEMORY), "cold OOM preserves exact typed cause");
    check(host.node() == Node<OverlayHost>{} && opener.node() == Node<Button>{} && first.node() == Node<Dialog>{},
          "failed mount publishes no Ref");
    check(take(f.app->tree().inspect(f.scene->root())).children.empty(), "failed cold graph leaves no subtree");
    // A failed attempt must not occupy the single-host/router budget.
    take(ui::mount(*f.scene, recipe(host, opener, first, inside, second)));
  }
  check(false, "mount allocation sweep reached fixture bound");
}

void runtimePublication(bool dismissing) {
  std::size_t beforeCommit{}, afterCommit{};
  for (std::size_t point = 0; point != MAX_FAILURE_POINTS; ++point) {
    Fixture f;
    ui::Ref<OverlayHost> host;
    ui::Ref<Button> opener, inside;
    ui::Ref<Dialog> first, second;
    take(ui::mount(*f.scene, recipe(host, opener, first, inside, second)));
    take(opener.focus());
    if (dismissing) {
      take(f.scene->openDialog(first.node()));
    }
    const auto before = take(f.app->tree().inspect(host.node())).children;
    (void)take(f.app->tree().consumeFrameRequest());
    allocationTest::remaining = static_cast<std::ptrdiff_t>(point);
    auto result = dismissing ? f.scene->dismissDialog(first.node()) : f.scene->openDialog(first.node());
    allocationTest::remaining = -1;
    const bool published = take(first.opened()) != dismissing;
    if (result) {
      check(published, "successful command publishes activation");
      std::cout << (dismissing ? "dismiss" : "open") << " failures before=" << beforeCommit << "; after=" << afterCommit
                << "; success index=" << point << '\n';
      return;
    }
    if (!treeCause(result.error(), TreeError::NO_MEMORY)) {
      const auto *actual = std::get_if<TreeError>(&result.error().cause);
      std::cerr << (dismissing ? "dismiss" : "open") << " failure point=" << point
                << "; phase=" << static_cast<int>(result.error().phase)
                << "; cause=" << (actual ? static_cast<int>(*actual) : -1) << "; published=" << published << '\n';
    }
    check(treeCause(result.error(), TreeError::NO_MEMORY), "runtime allocation failure remains typed");
    if (!published) {
      ++beforeCommit;
      check(take(f.app->tree().inspect(host.node())).children == before, "precommit OOM preserves sibling order");
      check(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE, "precommit OOM does not invalidate");
      check(take(f.app->router().focused()) == (dismissing ? inside.node() : opener.node()),
            "precommit OOM preserves focus");
    } else {
      ++afterCommit;
      check(take(f.app->router().focused()) == (dismissing ? opener.node() : inside.node()),
            "postpublication notification failure retains complete focus publication");
      check(take(f.app->tree().inspect(first.node())).visible == !dismissing,
            "postpublication failure retains complete visibility publication");
    }
  }
  check(false, "runtime allocation sweep reached fixture bound");
}

void owningCommandTransport() {
  static_assert(std::copy_constructible<ManagedError>);
  static_assert(std::copy_constructible<ui::DialogCommandResult>);

  struct UnsupportedCallback {
    ui::DialogCommandResult operator()() const;
  };

  static_assert(!portals::detail::ManagedAction<UnsupportedCallback, ClickEvent>);
  std::cout << "transport nothrow copy/move: error=" << std::is_nothrow_copy_constructible_v<ManagedError> << '/'
            << std::is_nothrow_move_constructible_v<ManagedError> << "; expected="
            << std::is_nothrow_copy_constructible_v<std::expected<void, ManagedError>> << '/'
            << std::is_nothrow_move_constructible_v<std::expected<void, ManagedError>> << "; facade="
            << std::is_nothrow_copy_constructible_v<ui::DialogCommandResult> << '/'
            << std::is_nothrow_move_constructible_v<ui::DialogCommandResult> << '\n';
  Fixture f;
  ui::Ref<Dialog> first, second;
  take(ui::mount(*f.scene, ui::overlayHost(ui::label("Body"), ui::dialog(first, "First", ui::label("one")),
                                           ui::dialog(second, "Second", ui::label("two")))));
  auto success = second.open();
  check(bool(success), "owning successful result");
  take(f.scene->remove(first.node()));
  auto failure = first.open();
  check(!failure && treeCause(failure.error(), TreeError::STALE_NODE), "owning failed result");
  allocationTest::remaining = 0;
  auto memory = second.dismiss();
  allocationTest::remaining = -1;
  check(!memory && treeCause(memory.error(), TreeError::NO_MEMORY), "owning allocation failure result");
  ui::Ref<Dialog> unpublished;
  auto unavailable = unpublished.open();
  std::optional<ui::DialogCommandResult> foreign;
  std::thread worker([&] {
    foreign.emplace(second.open());
  });
  worker.join();
  check(!unavailable && treeCause(unavailable.error(), TreeError::CLOSED) && foreign && !*foreign &&
          treeCause(foreign->error(), TreeError::WRONG_THREAD),
        "owning unavailable and foreign-thread results");
  f.scene.reset();
  const auto before = allocationTest::count;
  allocationTest::remaining = 0;
  const auto error = failure.error();
  const auto exact = failure.result();
  const auto copied = failure;
  const auto ready = success.result();
  const auto transport = [](const ui::DialogCommandResult &command, TreeError cause) {
    const auto error = command.error();
    const auto result = command.result();
    const auto copy = command;
    return treeCause(error, cause) && !result && !copy;
  };
  const bool allCauses = transport(memory, TreeError::NO_MEMORY) && transport(unavailable, TreeError::CLOSED) &&
                         transport(*foreign, TreeError::WRONG_THREAD);
  allocationTest::remaining = -1;
  check(allocationTest::count == before && treeCause(error, TreeError::STALE_NODE) && !exact && !copied && ready &&
          allCauses,
        "error/result copies allocate nothing and survive scene retirement");
}
} // namespace

int main() {
  try {
    mountRollback();
    runtimePublication(false);
    runtimePublication(true);
    owningCommandTransport();
    return 0;
  } catch (const std::exception &error) {
    allocationTest::remaining = -1;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
