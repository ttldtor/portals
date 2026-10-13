// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "style_pss_support.hpp"

using namespace stylePssConsumer;

/** @brief Explicit materialization and client-owned legacy live controller. */
struct Client {
  ManagedScene *owner{};
  sc::LiveTheme<Theme> live;

  /** @param scene Stationary facade borrowing this client until cleanup. */
  void install(ui::Scene &scene) {
    scene.styleLayers().configure([this](ManagedScene &managed) -> std::expected<void, ManagedError> {
      owner = &managed;

      const auto application = sc::materialize(APPLICATION);
      const auto user = sc::materialize(USER);
      const auto added = managed.addLayer(StyleTier::APPLICATION, "application", application);

      if (!added) {
        return std::unexpected(added.error());
      }

      const auto overridden = managed.addLayer(StyleTier::USER, "user", user);

      if (!overridden) {
        return std::unexpected(overridden.error());
      }

      return attachManaged();
    });
  }

  /** @return Original scalar value or live authority/lifetime failure. */
  auto value() const {
    return live.get<"width">();
  }

  /** @param width Candidate minimum width.
   * @return Original atomic update/no-change result or typed style failure.
   */
  auto width(std::int32_t width) {
    return live.set<"width">(width);
  }

  /** @param scene Same stationary facade; the saved managed owner remains borrowed.
   * @return Explicit new authority attachment or its original error.
   */
  std::expected<void, ManagedError> attach(ui::Scene &scene) {
    (void)scene;

    return attachManaged();
  }

  /** @return New legacy controller, retaining the existing evaluation and lease rules. */
  std::expected<void, ManagedError> attachManaged() {
    auto attached = sc::live(*owner, THEME);

    if (!attached) {
      return std::unexpected(attached.error());
    }

    live = *attached;

    return {};
  }
};

int main(int argc, char **argv) try { return run<Client>(argc, argv); } catch (const std::bad_alloc &) {
  std::cerr << "Stylesheet setup ran out of memory\n";

  return 1;
}
