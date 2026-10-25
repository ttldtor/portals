// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_delivery.hpp>

#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

// A headless customization consumer: application-owned work, immutable result, UI delivery.
// Delivery teardown never joins a worker; this finite test host joins before driving dispatch.
int main() {
  using namespace portals;
  auto app = Application::create();
  if (!app) {
    return 1;
  }

  auto scene = ManagedScene::create(**app);
  if (!scene) {
    return 2;
  }

  ui::Ref<Label> status;
  ui::Ref<Button> apply;
  int applied{};
  auto mounted = ui::mount(**scene, ui::column(ui::label(status, "Waiting"), ui::button(apply, "Apply").onClick([&] {
    ++applied;
  })));
  if (!mounted) {
    return 3;
  }

  auto endpoint = status.delivery<int>([status, apply](auto total) -> std::expected<void, TreeError> {
    auto written = status.text().set(std::to_string(*total)).result();
    if (!written) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }

    auto activated = apply.activate();
    if (!activated) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }

    return {};
  });
  if (!endpoint) {
    return 4;
  }

  const auto input = std::make_shared<const std::vector<int>>(std::vector<int>{1, 2, 3, 4});
  std::optional<UiAcknowledgement> acknowledgement;
  std::atomic<bool> rejected{};
  std::jthread worker([sender = *endpoint, input, &acknowledgement, &rejected] {
    try {
      const auto sum = std::accumulate(input->begin(), input->end(), 0);
      auto queued = sender.postAcknowledged(std::make_shared<const int>(sum));
      if (!queued) {
        // CLOSED, WOULD_BLOCK and NO_MEMORY are admission failures, never execution success.
        rejected = true;
        return;
      }

      acknowledgement = std::move(*queued);
    } catch (...) {
      rejected = true;
    }
  });
  worker.join();
  if (rejected || !acknowledgement || acknowledgement->poll().state != UiDeliveryState::PENDING) {
    return 5;
  }

  auto dispatched = (*app)->scheduler().dispatch();
  auto text = status.text().get();
  if (!dispatched || !text || *text != "10" || applied != 1 ||
      acknowledgement->poll().state != UiDeliveryState::APPLIED) {
    return 6;
  }

  // Closing the scene cancels future delivery; outstanding endpoint copies retain no UI captures.
  scene->reset();
  auto closed = endpoint->post(std::make_shared<const int>(20));
  return !closed && closed.error() == SchedulerError::CLOSED ? 0 : 7;
}
