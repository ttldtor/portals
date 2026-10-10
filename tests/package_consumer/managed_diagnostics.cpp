// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/managed.hpp>

#include <array>

// Existing exhaustive visitors must keep their original three alternatives.
static_assert(std::same_as<decltype(portals::ManagedError{}.cause),
                           std::variant<portals::TreeError, portals::StyleError, portals::ApplicationError>>);

int main() {
  using namespace portals;
  auto app = Application::create();

  if (!app) {
    return 1;
  }

  auto scene = ManagedScene::create(**app, {.diagnostics = {.maxErrors = 2}});

  if (!scene) {
    return 1;
  }

  std::size_t notifications{};
  auto observed = (*scene)->onError([&notifications](const ManagedDiagnostic &) {
    ++notifications;
  });

  if (!observed) {
    return 1;
  }

  const std::array context{ManagedDiagnosticContext{.operation = "append", .name = "message"}};
  const ManagedError failure{
    .phase = ManagedPhase::INPUT, .node = (*scene)->root().id(), .cause = TreeError::STALE_NODE};

  for (std::size_t index = 0; index < 3; ++index) {
    if (!(*scene)->reportError(failure, context)) {
      return 1;
    }
  }

  if (!(*scene)->close()) {
    return 1;
  }

  auto errors = (*scene)->drainErrors();

  if (!errors || !errors->closed || errors->errors.size() != 2 || errors->droppedErrors != 1 || notifications != 3) {
    return 1;
  }

  const auto &first = errors->errors.front();
  auto empty = (*scene)->diagnostics();

  return first.contexts.size() == 1 && first.contexts.front().name == "message" &&
             std::get<TreeError>(first.error.cause) == TreeError::STALE_NODE && empty && empty->errors.empty()
           ? 0
           : 1;
}
