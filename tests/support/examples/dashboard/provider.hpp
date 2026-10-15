// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "../../../../examples/dashboard/provider.hpp"

namespace dashboard_test {
using namespace portals;
using namespace dashboard;

struct Options {
  /** @brief Borrowed callback sampling this scene's snapshot ingress latency in scheduler time units. */
  using IngressObserver = void (*)(void *, SchedulerTime);
  /** @brief Optional observer; when null, ingress performs no sampling work. */
  IngressObserver ingressObserver{};
  /** @brief Borrowed observer context, required to remain valid through scene shutdown. */
  void *ingressContext{};
  /** @brief Number of logical instruments exposed by the virtual model. */
  std::uint64_t rows{1000000};
  /** @brief Synthetic or replay ticks generated per second of application clock time. */
  std::uint32_t rate{1000};
  /** @brief Seed for deterministic synthetic prices. */
  std::uint64_t seed{42};
  /** @brief Maximum tick work in one provider pump after a clock jump. */
  std::size_t maxTicksPerPump{512};
  /** @brief Additional delay in milliseconds before a requested range becomes ready. */
  std::uint64_t delayMs{};
  /** @brief Finite synthetic page availability, payload and prefetch budgets. */
  LoadingOptions loading;
  /** @brief Optional run duration in milliseconds; zero leaves stopping to input or the caller. */
  std::uint64_t durationMs{};
  /** @brief Use the fixed replay prices instead of the synthetic generator. */
  bool replay{};
  /** @brief Select ASCII captions and chart glyphs for limited terminals. */
  bool ascii{};
  /** @brief Layout preference retained for standalone scene adapters; the provider ignores presentation. */
  bool stress{true};

  operator dashboard::Options() const {
    return {.rows = rows,
            .rate = rate,
            .seed = seed,
            .maxTicksPerPump = maxTicksPerPump,
            .delayMs = delayMs,
            .loading = loading,
            .durationMs = durationMs,
            .replay = replay,
            .ascii = ascii};
  }
};

/** @brief Test-owned latency observation and stress-layout options over the real provider. */
class Provider : public dashboard::Provider {
public:
  Provider(Application &app, Options options)
      : dashboard::Provider(app, static_cast<dashboard::Options>(options)), app_(app), options_(options) {
  }

  void start(ManagedModel<Quote> model) {
    dashboard::Provider::start(
      model, [observer = options_.ingressObserver, context = options_.ingressContext, scheduler = &app_.scheduler()] {
        if (observer) {
          observer(context, scheduler->metrics().data.last_latency);
        }
      });
  }

private:
  Application &app_;
  Options options_;
};
} // namespace dashboard_test
