// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "theme_selection_state.hpp"

#include <portals/ui_scene.hpp>

namespace portals::detail {
struct PreparedThemeIntent {
  std::size_t choice{};
  std::shared_ptr<const std::function<std::expected<StyleSheet, StyleError>(std::stop_token)>> source;
  StyleThemeLease authority;
  std::weak_ptr<const void> engine;
  std::source_location origin;
};

struct PreparedThemeResult {
  std::shared_ptr<const PreparedThemeIntent> intent;
  StyleSheet sheet;
  std::optional<StyleError> styleError;
  std::optional<AsyncError> asyncError;
};

// Account the same logical rule/declaration payload on the worker before UI copies.
inline bool preparedThemeFits(const StyleSheet &sheet, const StyleLayerOptions &limits) noexcept {
  if (sheet.rules.size() > limits.maxRules) {
    return false;
  }

  std::size_t bytes{};
  std::size_t declarations{};
  const auto charge = [](std::size_t &used, std::size_t amount, std::size_t limit) {
    if (amount > limit - used) {
      return false;
    }

    used += amount;

    return true;
  };
  const auto text = [&](std::string_view value) {
    return charge(bytes, value.size(), limits.maxOwnedBytes) && charge(bytes, 1, limits.maxOwnedBytes);
  };
  const auto paint = [](const StylePaintDeclarations &part) {
    return part.foreground.has_value() + part.background.has_value() + part.bold.has_value() + part.italic.has_value() +
           part.underline.has_value();
  };

  for (const auto &rule : sheet.rules) {
    const auto &d = rule.declarations;
    const std::size_t count = paint(d.frameLine) + paint(d.frameTitle) + paint(d.frameEnds) + paint(d.scrollbarTrack) +
                              paint(d.scrollbarThumb) + paint(d.scrollbarEnds) + paint(d.scrollbarCorner) +
                              d.foreground.has_value() + d.background.has_value() + d.bold.has_value() +
                              d.italic.has_value() + d.underline.has_value() + d.minWidth.has_value();

    if (!charge(declarations, count, limits.maxDeclarations) ||
        !charge(bytes, sizeof(StyleRule), limits.maxOwnedBytes) || !text(rule.selector.type) ||
        !text(rule.selector.id)) {
      return false;
    }

    for (const auto &name : rule.selector.classes) {
      if (!charge(bytes, sizeof(std::string), limits.maxOwnedBytes) || !text(name)) {
        return false;
      }
    }
  }

  return true;
}

class PreparedThemeJob final : public BackgroundJob {
public:
  using Factory = std::function<std::expected<StyleSheet, StyleError>(std::stop_token)>;

  PreparedThemeJob(std::shared_ptr<const Factory> factory, std::shared_ptr<PreparedThemeResult> result,
                   Publisher<PreparedThemeResult> publisher, StyleLayerOptions limits)
      : factory_(std::move(factory)), result_(std::move(result)), publisher_(std::move(publisher)), limits_(limits) {
  }

  void run(std::stop_token stop) noexcept override {
    try {
      // The prototype is never invoked. Each attempt copies its closure on the worker,
      // so overlapping cancelled attempts do not share a mutable callable instance.
      auto factory = *factory_;
      auto produced = factory(stop);

      if (stop.stop_requested()) {
        result_->asyncError = AsyncError::CANCELLED;
      } else if (!produced) {
        result_->styleError = produced.error();
      } else if (!preparedThemeFits(*produced, limits_)) {
        result_->styleError = StyleError{TreeError::INVALID_ARGUMENT, 1, 1, "Prepared theme payload budget exhausted"};
      } else {
        result_->sheet.rules.swap(produced->rules);
        result_->sheet.scope = produced->scope;
      }
    } catch (const std::bad_alloc &) {
      result_->asyncError = AsyncError::NO_MEMORY;
    } catch (...) {
      result_->asyncError = AsyncError::CALLBACK_FAILED;
    }
  }

  std::expected<void, AsyncError> publish() override {
    const auto sent = publisher_.publish(result_);

    if (!sent) {
      return std::unexpected(portals::detail::asyncError(sent.error()));
    }

    return {};
  }

private:
  std::shared_ptr<const Factory> factory_;
  std::shared_ptr<PreparedThemeResult> result_;
  Publisher<PreparedThemeResult> publisher_;
  StyleLayerOptions limits_;
};

// UI-only activity. Workers retain only the Publisher and worker-safe job storage.
struct ThemePreparation final : ui::detail::SceneActivity {
  Application *application{};
  std::weak_ptr<const void> lifetime;
  UiScope scope;
  Subscription subscription;
  Publisher<PreparedThemeResult> publisher;
  std::unique_ptr<BackgroundLatest> latest;
  std::unique_ptr<BackgroundJob> deferred;
  std::function<void(std::optional<BackgroundError>, std::optional<AsyncError>)> failed;
  bool closed{};
  bool hasWork{};

  bool active() const noexcept override {
    return hasWork && !closed && !lifetime.expired();
  }

  std::expected<void, BackgroundError> replace(std::unique_ptr<BackgroundJob> job) {
    cancelWork();
    deferred = std::move(job);
    hasWork = true;

    return admit();
  }

  std::expected<void, BackgroundError> admit() {
    auto *executor = application->background();

    // Scene mounting precedes the default executor startup. Preserve initial intent.
    if (!executor) {
      return {};
    }

    if (!latest) {
      latest = std::make_unique<BackgroundLatest>(*executor);
    }

    auto result = deferred ? latest->replace(std::move(deferred)) : latest->service();

    if (!result) {
      return std::unexpected(result.error());
    }

    return {};
  }

  void service() noexcept override {
    if (!active()) {
      return;
    }

    try {
      auto result = admit();

      if (result) {
        return;
      }

      cancelWork();
      failed(result.error(), {});
    } catch (const std::bad_alloc &) {
      cancelWork();
      failed({}, AsyncError::NO_MEMORY);
    } catch (...) {
      cancelWork();
      failed({}, AsyncError::CALLBACK_FAILED);
    }
  }

  void cancelWork() noexcept {
    hasWork = false;

    if (latest) {
      latest->cancel();
    }

    deferred.reset();
  }

  void cancel() noexcept override {
    closed = true;
    cancelWork();
    (void)scope.close();
  }
};
} // namespace portals::detail
