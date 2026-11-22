// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "document_view_state.hpp"
#include <portals/background.hpp>
#include <portals/detail/ui_recipe_access.hpp>
#include <portals/document_search.hpp>
#include <portals/ui_document_export.hpp>
#include <portals/ui_scene.hpp>

namespace portals::ui {
namespace {
ManagedError exportFailure(Node<DocumentView> node, const DocumentError &error) {
  const auto bridge = error.code == SourceError::NO_MEMORY     ? TreeError::NO_MEMORY
                      : error.code == SourceError::WOULD_BLOCK ? TreeError::WOULD_BLOCK
                      : error.code == SourceError::CLOSED      ? TreeError::CLOSED
                                                               : TreeError::INVALID_ARGUMENT;
  ManagedError failure{ManagedPhase::BIND, node.id(), bridge, {}, {}};
  failure.operation = "DocumentView export";

  failure.source = error.code;
  failure.logFile = error.fileFailure;
  if (error.code != SourceError::NO_MEMORY) {
    try {
      failure.document = std::make_shared<const DocumentOperationFailure>(DocumentOperationFailure{error, {}});
    } catch (const std::bad_alloc &) {
      // The original allocation-free cause remains available.
    }
  }
  return failure;
}

struct ExportCompletion {
  SourceCredit credit;
  DocumentExportResult result;
};

class ExportJob final : public BackgroundJob {
public:
  ExportJob(std::shared_ptr<ByteDocumentSource> source, std::shared_ptr<DocumentExportSink> sink,
            Publisher<DocumentExportResult> publisher, SourceCredit credit)
      : source_(std::move(source)), sink_(std::move(sink)), publisher_(std::move(publisher)),
        result_(std::make_shared<ExportCompletion>()) {
    result_->credit = std::move(credit);
  }

  void run(std::stop_token stop) noexcept override {
    try {
      result_->result = exportDocument(source_, sink_, {}, {}, stop);
    } catch (const std::bad_alloc &) {
      result_->result.error = DocumentError{SourceError::NO_MEMORY, {}, {}};
    } catch (...) {
      result_->result.error = DocumentError{SourceError::CALLBACK_FAILED, {}, {}};
    }
  }

  std::expected<void, AsyncError> publish() override {
    const auto published = publisher_.publish(std::shared_ptr<const DocumentExportResult>(result_, &result_->result));

    if (published) {
      return {};
    }

    return std::unexpected(published.error() == SchedulerError::WOULD_BLOCK ? AsyncError::WOULD_BLOCK
                           : published.error() == SchedulerError::NO_MEMORY ? AsyncError::NO_MEMORY
                                                                            : AsyncError::CLOSED);
  }

private:
  std::shared_ptr<ByteDocumentSource> source_;
  std::shared_ptr<DocumentExportSink> sink_;
  Publisher<DocumentExportResult> publisher_;
  std::shared_ptr<ExportCompletion> result_;
};

class ExportActivity final : public detail::SceneActivity, public std::enable_shared_from_this<ExportActivity> {
public:
  ExportActivity(ManagedScene &scene, Node<DocumentView> node, std::weak_ptr<portals::detail::DocumentViewState> state,
                 std::function<void(const DocumentExportResult &)> callback, std::source_location origin)
      : scene_(scene), node_(node), lifetime_(detail::Access::lifetime(scene)), state_(std::move(state)),
        callback_(std::move(callback)), origin_(origin) {
  }

  bool active() const noexcept override {
    return !closed_ && !lifetime_.expired() && !state_.expired();
  }

  void cancel() noexcept override {
    closed_ = true;

    if (latest_) {
      latest_->cancel();
    }

    (void)connection_.subscription.disconnect();
    completion_.reset();

    if (auto state = state_.lock(); state && state->exporting && state->exporting->activity.get() == this) {
      state->exporting.reset();
    }
  }

  std::expected<void, ManagedError> start(std::shared_ptr<ByteDocumentSource> source,
                                          std::shared_ptr<DocumentExportSink> sink) {
    auto &app = detail::SceneAccess::application(scene_);
    auto *executor = app.background();

    if (!executor) {
      return std::unexpected(detail::error(node_, TreeError::INVALID_ARGUMENT));
    }

    std::expected<SchedulerConnection<DocumentExportResult>, SchedulerError> connected;
    auto scoped = app.tree().withScope(node_, [&](const UiScope &scope) {
      connected = app.scheduler().connect<DocumentExportResult>(
        scope, [weak = weak_from_this()](std::shared_ptr<const DocumentExportResult> result) {
          if (auto owner = weak.lock(); owner && owner->active()) {
            owner->completion_ = std::move(result);
          }
        });
    });

    if (!scoped || !connected) {
      return std::unexpected(detail::error(node_, !scoped ? scoped.error()
                                                  : connected.error() == SchedulerError::NO_MEMORY
                                                    ? TreeError::NO_MEMORY
                                                    : TreeError::CLOSED));
    }

    connection_ = std::move(*connected);
    latest_ = std::make_unique<BackgroundLatest>(*executor);
    constexpr std::size_t EXPORT_METADATA_BYTES = 4096;
    auto credit = source->budget()->reserve({EXPORT_METADATA_BYTES, 0, 0, 0});

    if (!credit) {
      DocumentError failure{credit.error(), {}, {}};
      failure.budgetDemand = SourceBudgetUsage{EXPORT_METADATA_BYTES, 0, 0, 0};
      return std::unexpected(exportFailure(node_, failure));
    }

    auto admitted = latest_->replace(
      std::make_unique<ExportJob>(std::move(source), std::move(sink), connection_.publisher, std::move(*credit)));

    if (!admitted) {
      return std::unexpected(detail::error(
        node_, admitted.error() == BackgroundError::NO_MEMORY ? TreeError::NO_MEMORY : TreeError::WOULD_BLOCK));
    }

    return {};
  }

  void service() noexcept override {
    auto retained = shared_from_this();

    try {
      serviceReady();
    } catch (const std::bad_alloc &) {
      cancel();
      if (!lifetime_.expired()) {
        (void)scene_.reportError(detail::error(node_, TreeError::NO_MEMORY), {}, origin_);
      }
    } catch (...) {
      cancel();
      if (!lifetime_.expired()) {
        (void)scene_.reportError(detail::error(node_, TreeError::CALLBACK_FAILED), {}, origin_);
      }
    }
  }

private:
  void serviceReady() {
    if (!active()) {
      cancel();
      return;
    }

    const auto result = latest_->service();

    if (!result) {
      cancel();
      (void)scene_.reportError(detail::error(node_, result.error() == BackgroundError::NO_MEMORY
                                                      ? TreeError::NO_MEMORY
                                                      : TreeError::WOULD_BLOCK),
                               {}, origin_);
      return;
    }

    auto completion = std::exchange(completion_, {});

    if (!completion) {
      return;
    }

    // Copy every callback/input before invoking host code: it may close the scene.
    auto callback = std::move(callback_);
    cancel();

    try {
      if (callback) {
        callback(*completion);
      }
    } catch (const std::bad_alloc &) {
      if (!lifetime_.expired()) {
        (void)scene_.reportError(detail::error(node_, TreeError::NO_MEMORY), {}, origin_);
      }
      return;
    } catch (...) {
      if (!lifetime_.expired()) {
        (void)scene_.reportError(detail::error(node_, TreeError::CALLBACK_FAILED), {}, origin_);
      }
      return;
    }

    if (!lifetime_.expired() && completion->error) {
      (void)scene_.reportError(exportFailure(node_, *completion->error), {}, origin_);
    }
  }

private:
  ManagedScene &scene_;
  Node<DocumentView> node_;
  std::weak_ptr<const void> lifetime_;
  std::weak_ptr<portals::detail::DocumentViewState> state_;
  std::function<void(const DocumentExportResult &)> callback_;
  std::unique_ptr<BackgroundLatest> latest_;
  SchedulerConnection<DocumentExportResult> connection_;
  std::shared_ptr<const DocumentExportResult> completion_;
  std::source_location origin_;
  bool closed_{};
};
} // namespace

std::expected<void, ManagedError> startExport(Ref<DocumentView> viewer, std::filesystem::path output,
                                              std::function<void(const DocumentExportResult &)> completed,
                                              std::source_location origin) {
  const auto target = detail::NamedAccess::target(viewer);
  auto scene = detail::eventScene(target);

  if (!scene) {
    return std::unexpected(scene.error());
  }

  const auto node = viewer.node();

  try {
    std::shared_ptr<portals::detail::DocumentViewState> state;
    const auto read = detail::SceneAccess::application(**scene).tree().read(node, [&](const DocumentView &widget) {
      state = portals::detail::DocumentViewAccess::shared(widget);
    });

    if (!read || !state || !state->source) {
      return std::unexpected(detail::error(node, !read ? read.error() : TreeError::INVALID_ARGUMENT));
    }

    if (state->exporting && state->exporting->activity && state->exporting->activity->active()) {
      return std::unexpected(detail::error(node, TreeError::WOULD_BLOCK));
    }

    auto presentation = std::make_unique<portals::detail::DocumentExportPresentation>();
    auto sink = DocumentFileSink::create(std::move(output));

    if (!sink) {
      return std::unexpected(exportFailure(node, sink.error()));
    }

    if (completed) {
      completed = detail::currentSceneCallback(detail::SceneAccess::callbackContext(**scene), std::move(completed));
    }

    auto activity = std::make_shared<ExportActivity>(**scene, node, state, std::move(completed), origin);
    auto admitted = detail::SceneAccess::admitActivity(**scene, activity);

    if (!admitted) {
      return admitted;
    }

    auto started = activity->start(state->source, std::move(*sink));

    if (!started) {
      activity->cancel();
    }

    if (started) {
      presentation->activity = activity;
      state->exporting = std::move(presentation);
    }

    return started;
  } catch (const std::bad_alloc &) {
    return std::unexpected(detail::error(node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(detail::error(node, TreeError::CALLBACK_FAILED));
  }
}

bool exportFile(Ref<DocumentView> viewer, std::filesystem::path output,
                std::function<void(const DocumentExportResult &)> completed, std::source_location origin) {
  auto target = detail::NamedAccess::target(viewer);
  auto result = startExport(std::move(viewer), std::move(output), std::move(completed), origin);

  if (!result) {
    detail::reportEventError(target, result.error(), origin);
  }

  return result.has_value();
}
} // namespace portals::ui
