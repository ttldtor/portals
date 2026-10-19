// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <cstdlib>
#include <iostream>
#include <new>
#include <portals/ui.hpp>
#include <source_location>
#include <stdexcept>
#include <utility>

namespace allocationTest {
thread_local std::ptrdiff_t remaining = -1;
thread_local void (*callback)(void *){};
thread_local void *context{};
} // namespace allocationTest

void *operator new(std::size_t size) {
  if (allocationTest::callback && allocationTest::remaining-- == 0) {
    auto callback = std::exchange(allocationTest::callback, nullptr);
    callback(allocationTest::context);
  }
  if (auto *p = std::malloc(size ? size : 1)) {
    return p;
  }
  throw std::bad_alloc();
}

void operator delete(void *p) noexcept {
  std::free(p);
}

void operator delete(void *p, std::size_t) noexcept {
  std::free(p);
}

void *operator new[](std::size_t n) {
  return ::operator new(n);
}

void operator delete[](void *p) noexcept {
  std::free(p);
}

void operator delete[](void *p, std::size_t) noexcept {
  std::free(p);
}

void *operator new(std::size_t n, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(n);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t n, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(n);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *p, const std::nothrow_t &) noexcept {
  std::free(p);
}

void operator delete[](void *p, const std::nothrow_t &) noexcept {
  std::free(p);
}

namespace {
using namespace portals;

void check(bool value, const char *message, std::source_location where = std::source_location::current()) {
  if (!value) {
    throw std::runtime_error(std::string(message) + " at " + std::to_string(where.line()));
  }
}

template <class T, class E> T take(std::expected<T, E> result) {
  check(bool(result), "unexpected checked failure");
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

Node<Widget> button(WidgetTree &tree, Node<Widget> root) {
  bool found{};
  take(tree.read(root, [&](const Widget &widget) {
    found = dynamic_cast<const Button *>(&widget) != nullptr;
  }));
  if (found) {
    return root;
  }
  for (auto child : take(tree.inspect(root)).children) {
    auto result = button(tree, child);
    if (result != Node<Widget>{}) {
      return result;
    }
  }
  return {};
}

struct Removal {
  WidgetTree *tree;
  Node<Widget> target;
  bool fired{}, removed{};

  static void invoke(void *context) {
    auto &self = *static_cast<Removal *>(context);
    self.fired = true;
    self.removed = bool(self.tree->remove(self.target));
    (void)self.tree->consumeFrameRequest();
  }
};

void staging(int target) {
  for (std::size_t point = 0; point < 1024; ++point) {
    auto app = take(Application::create());
    auto scene = take(ManagedScene::create(*app));
    ui::MessageBox box;
    ui::Ref<OverlayHost> host;
    ui::Ref<Column> owner;
    ui::Ref<Dialog> dialog;
    take(ui::mount(*scene, ui::overlayHost(host, ui::column(ui::column(ui::label("Form")).reference(owner)),
                                           ui::unsavedChangesDialog(box, "Unsaved", "Body").reference(dialog))));
    const auto panel = take(app->tree().inspect(dialog.node())).children.front();
    const auto content = take(app->tree().inspect(panel)).children.front();
    const auto body = take(app->tree().inspect(content)).children.back();
    Removal removal{&app->tree(), target == 0   ? Node<Widget>(owner.node())
                                  : target == 1 ? Node<Widget>(dialog.node())
                                  : target == 2 ? body
                                  : target == 3 ? button(app->tree(), dialog.node())
                                                : Node<Widget>(host.node())};
    int callbacks{};
    allocationTest::remaining = static_cast<std::ptrdiff_t>(point);
    allocationTest::context = &removal;
    allocationTest::callback = &Removal::invoke;
    auto opened = box.open(owner, [&] {
      ++callbacks;
    });
    allocationTest::callback = nullptr;
    if (!removal.fired) {
      check(bool(opened), "eventual unstressed admission");
      std::cout << "message reentry target=" << target << " allocation points=" << point << '\n';
      return;
    }
    check(removal.removed && !opened, "allocation reentry cannot publish through retired original identity");
    if (target == 0 || target == 2 || target == 3) {
      auto visible = scene->dialogOpened(dialog.node());
      check(visible && !*visible, "failed prepublication keeps panel closed");
    }
    take(app->scheduler().dispatch());
    check(callbacks == 0, "unadopted allocation-reentry request cannot deliver");
  }
  check(false, "reentry sweep exceeded finite cap");
}
} // namespace
int main() try {
  for (int target = 0; target < 5; ++target) {
    staging(target);
  }
  std::cout << "message-box-reentry: owner/dialog/body/button/host staging PASS\n";
} catch (const std::exception &error) {
  allocationTest::callback = nullptr;
  std::cerr << error.what() << '\n';
  return 1;
}
