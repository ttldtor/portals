// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>

namespace allocationTest {
thread_local std::ptrdiff_t remaining = -1;
thread_local std::size_t count{};
} // namespace allocationTest

void *operator new(std::size_t size) {
  if (allocationTest::remaining == 0) {
    throw std::bad_alloc();
  }
  if (allocationTest::remaining > 0) {
    --allocationTest::remaining;
  }
  ++allocationTest::count;
  if (auto *result = std::malloc(size ? size : 1)) {
    return result;
  }
  throw std::bad_alloc();
}

void operator delete(void *value) noexcept {
  std::free(value);
}

void operator delete(void *value, std::size_t) noexcept {
  std::free(value);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void *value) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  ::operator delete(value);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

namespace {
using namespace portals;
constexpr CellSize VIEWPORT{30, 4};

void check(bool condition, std::string_view message, std::source_location where = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error(std::string(message) + " at line " + std::to_string(where.line()));
  }
}

template <class T, class E>
T take(std::expected<T, E> result, std::source_location where = std::source_location::current()) {
  if (!result) {
    if constexpr (std::same_as<E, ManagedError>) {
      if (const auto *cause = std::get_if<TreeError>(&result.error().cause)) {
        std::cerr << "managed phase=" << static_cast<int>(result.error().phase)
                  << " tree cause=" << static_cast<int>(*cause) << '\n';
      }
    }
  }
  check(result.has_value(), "unexpected API failure", where);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool treeCause(const ManagedError &failure, TreeError cause) {
  const auto *value = std::get_if<TreeError>(&failure.cause);
  return value && *value == cause;
}

struct Fixture {
  example::ReplayTerminal terminal{{}, 5000, VIEWPORT};
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;

  explicit Fixture(ManagedOptions managed = {}, std::size_t capacity = 8) {
    ApplicationOptions options;
    options.scheduler.data_capacity = capacity;
    app = take(Application::create(options, terminal.clock()));
    scene = take(ManagedScene::create(*app, managed));
  }

  void prepare() {
    take(scene->prepare(*app, VIEWPORT));
  }

  void dispatch() {
    take(app->scheduler().dispatch());
  }

  Node<Widget> node() {
    return take(scene->emplace<Widget>(scene->root()));
  }
};

void prepareViews(Fixture &fixture, CellSize size = VIEWPORT) {
  std::optional<LayoutSnapshot> layout;
  take(fixture.app->tree().frame([&] {
    const auto bounds = take(CellRect::from({0, 0}, size));
    take(layoutTree(fixture.app->tree(), take(fixture.app->tree().root()), bounds, bounds));
    layout = take(LayoutSnapshot::capture(fixture.app->tree(), fixture.scene->root()));
  }));
  take(fixture.scene->prepareViews(*fixture.app, *layout));
}
} // namespace
