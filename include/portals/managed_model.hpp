// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/chart.hpp>
#include <portals/detail/managed_origin.hpp>

namespace portals {
namespace ui::detail {
struct Access;
}
class ManagedScene;

/** @brief Owning callback after each successful virtual-view preparation, including layout retries.
 * Both arguments are borrowed for this invocation. Copy a request to retain provider work;
 * use its original typed completion/retry API. Do not dispatch the scheduler, repin models,
 * or mutate the managed registry here. Low-level presentation changes are allowed.
 * @tparam T Immutable domain row shared by the model and view.
 */
template <class T>
using ManagedViewHandler =
  std::function<std::expected<void, TreeError>(const ModelFrame<T> &, const std::optional<ModelRequest<T>> &)>;

/// @cond INTERNAL
namespace detail {
struct ManagedModelSource {
  virtual ~ManagedModelSource() = default;
  virtual std::expected<void, TreeError> pin() = 0;
  virtual bool bound() const noexcept = 0;
  virtual std::expected<bool, TreeError> synchronize() = 0;
  virtual void retire() noexcept = 0;
  std::shared_ptr<ManagedOrigin> origin;
  bool retired{};
  Node<Widget> receiver;
};

template <class T> struct ManagedPreparedRegistration {
  std::shared_ptr<ManagedViewHandler<T>> action;
};

template <class T> struct ManagedModelSourceOf final : ManagedModelSource {
  std::unique_ptr<Model<T>> model;
  std::optional<ModelFrame<T>> frame;
  NodeId scene;
  bool sinkOnly{};
  std::function<std::expected<bool, TreeError>()> sync;
  std::vector<std::weak_ptr<ManagedPreparedRegistration<T>>> prepared =
    std::vector<std::weak_ptr<ManagedPreparedRegistration<T>>>(0);

  bool bound() const noexcept override {
    return sinkOnly;
  }

  void retire() noexcept override {
    retired = true;
    if (model) {
      auto latest = model->pin();
      if (latest) {
        frame = std::move(*latest);
      }
    }
    // Registration ownership ends immediately. An admitted caller retains its separate action owner.
    for (const auto &weak : prepared) {
      if (auto registration = weak.lock()) {
        registration->action.reset();
      }
    }
    prepared.clear();
    sync = {};
    model.reset();
  }

  std::expected<bool, TreeError> synchronize() override {
    return sync ? sync() : std::expected<bool, TreeError>(false);
  }

  std::expected<void, TreeError> pin() override {
    if (retired) {
      return {};
    }
    auto value = model->pin();

    if (!value) {
      return std::unexpected(value.error());
    }

    frame = std::move(*value);

    return {};
  }
};
} // namespace detail

/// @endcond

/** @brief Weak UI handle to a model owned by one ManagedScene.
 * Handles may be copied and destroyed on any thread. Operations require the creating
 * UI thread. Closing the scene closes its model endpoints; handles do not extend ownership.
 * @tparam T Immutable domain row satisfying Model<T>'s snapshot and destruction contract.
 */
template <class T> class ManagedModel {
public:
  /// @brief Construct an empty handle whose operations return CLOSED.
  ManagedModel() = default;

  /** @brief Copy the existing worker-safe publication endpoint on UI.
   * @return Publisher for newer snapshots, WRONG_THREAD, or CLOSED after scene cleanup.
   * Collection-bound sink-only models return WOULD_BLOCK instead of exposing a writable endpoint.
   * A rejected publication remains the provider's responsibility; retain and retry its value.
   */
  std::expected<Publisher<ModelSnapshot<T>>, TreeError> publisher() const {
    auto source = lock();

    if (source && (*source)->sinkOnly) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }

    return source ? std::expected<Publisher<ModelSnapshot<T>>, TreeError>((*source)->model->publisher())
                  : std::unexpected(source.error());
  }

private:
  friend class ManagedScene;
  friend struct ui::detail::Access;

  std::expected<std::shared_ptr<detail::ManagedModelSourceOf<T>>, TreeError> lock() const {
    if (owner_ != std::thread::id{} && owner_ != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }

    auto source = source_.lock();

    if (!source) {
      return std::unexpected(TreeError::CLOSED);
    }
    if (source->origin) {
      auto live = source->origin->check();
      if (!live) {
        return std::unexpected(live.error());
      }
    }

    return source;
  }

  std::weak_ptr<detail::ManagedModelSourceOf<T>> source_;
  std::thread::id owner_{};
};

/** @brief Owning formatter of a borrowed visible list row into owning text.
 * The callback runs during preparation on UI and must not retain the borrowed row.
 * @tparam T Immutable domain row shared by the model and list.
 */
template <class T> using ManagedListFormatter = std::function<GraphemeText(const ModelRow<T> &)>;

/** @brief Owning projection of a borrowed domain row into an optional candle.
 * The callback runs during preparation on UI; nullopt introduces a chart gap.
 * It must not retain the borrowed row.
 * @tparam T Immutable domain row shared by the model and chart.
 */
template <class T> using ManagedChartAccessor = std::function<std::optional<Ohlc>(const T &)>;

/** @brief Owning chart-origin callback, evaluated in registration order before chart preparation.
 * @return First model row or an original TreeError; empty callback means row zero.
 */
using ManagedChartOrigin = std::function<std::expected<std::uint64_t, TreeError>()>;
} // namespace portals
