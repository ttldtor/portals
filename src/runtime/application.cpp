// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../widgets/scroll_controls_state.hpp"
#include "../widgets/scroll_preparation_state.hpp"
#include <portals/application.hpp>
#include <portals/background.hpp>

#include <algorithm>
#include <thread>
#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

namespace portals {
namespace {
struct Failure {
  ApplicationCause cause;
  std::optional<BackgroundError> background{};
};

template <class T, class E> T require(std::expected<T, E> value) {
  if (!value) {
    if constexpr (std::same_as<E, BackgroundError>) {
      throw Failure{ApplicationFailure::BACKGROUND_FAILED, value.error()};
    } else {
      throw Failure{value.error(), {}};
    }
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

void captureFailure(ApplicationPhase phase, std::optional<ApplicationError> &error) {
  if (error) {
    return;
  }

  try {
    throw;
  } catch (const Failure &failure) {
    error.emplace(ApplicationError{phase, failure.cause, {}, failure.background});
  } catch (const std::bad_alloc &) {
    error.emplace(ApplicationError{phase, ApplicationFailure::NO_MEMORY, {}});
  } catch (...) {
    error.emplace(ApplicationError{phase, ApplicationFailure::CALLBACK_FAILED, {}});
  }
}

TerminalDeadline deadlineAt(SchedulerTime time) {
  return TerminalDeadline(std::chrono::milliseconds(time));
}

class SurfacePaint final : public PaintContext {
public:
  SurfacePaint(NodeGeometry geometry, std::vector<RenderCommand> &commands, std::size_t limit)
      : geometry_(geometry), commands_(commands), limit_(limit) {
  }

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    if (commands.size() > limit_ - commands_.size()) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    for (const auto &command : commands) {
      auto result = std::visit(
        [&](const auto &value) -> std::expected<void, TreeError> {
          auto clip = value.clip.translated(geometry_.surface.origin());

          if (!clip) {
            return std::unexpected(TreeError::INVALID_GEOMETRY);
          }

          if constexpr (std::same_as<std::decay_t<decltype(value)>, TextCommand>) {
            auto origin = translated(value.origin, geometry_.surface.origin());

            if (!origin) {
              return std::unexpected(TreeError::INVALID_GEOMETRY);
            }

            commands_.emplace_back(
              TextCommand{*origin, value.text, clip->intersection(geometry_.clip), value.style, value.z});
          } else {
            auto area = value.area.translated(geometry_.surface.origin());

            if (!area) {
              return std::unexpected(TreeError::INVALID_GEOMETRY);
            }

            commands_.emplace_back(FillCommand{*area, clip->intersection(geometry_.clip), value.style, value.z});
          }

          return {};
        },
        command);

      if (!result) {
        return result;
      }
    }

    return {};
  }

private:
  NodeGeometry geometry_;
  std::vector<RenderCommand> &commands_;
  std::size_t limit_;
};

void paintTree(WidgetTree &tree, Node<Widget> node, std::vector<RenderCommand> &commands, std::size_t limit) {
  auto snapshot = require(tree.inspect(node));

  if (!snapshot.visible || !snapshot.geometry.valid) {
    return;
  }

  SurfacePaint sink(snapshot.geometry, commands, limit);

  require(tree.paint(node, sink));

  for (auto child : snapshot.children) {
    paintTree(tree, child, commands, limit);
  }
}

DiagnosticInput inputCategory(const InputEvent &event) {
  return std::visit(
    [](const auto &value) {
      using T = std::decay_t<decltype(value)>;

      if constexpr (std::same_as<T, KeyEvent>) {
        return DiagnosticInput::KEY;
      } else if constexpr (std::same_as<T, TextInputEvent>) {
        return DiagnosticInput::TEXT;
      } else if constexpr (std::same_as<T, PasteInputEvent>) {
        return DiagnosticInput::PASTE;
      } else if constexpr (std::same_as<T, MouseInputEvent>) {
        return DiagnosticInput::MOUSE;
      }

      else {
        return DiagnosticInput::OTHER;
      }
    },
    event);
}
} // namespace

LayoutSnapshot::LayoutSnapshot(std::uint64_t session, std::shared_ptr<const std::vector<Record>> records)
    : session_(session), records_(std::move(records)) {
}

LayoutSnapshot::LayoutSnapshot(std::uint64_t session, std::shared_ptr<const std::vector<Record>> records,
                               std::shared_ptr<const std::vector<GeometryRecord>> geometry, std::uint64_t revision,
                               std::uint64_t inputGeneration)
    : session_(session), records_(std::move(records)), geometry_(std::move(geometry)), geometryRevision_(revision),
      inputGeneration_(inputGeneration) {
}

std::expected<LayoutSnapshot, TreeError> LayoutSnapshot::capture(WidgetTree &tree, Node<Widget> node) {
  try {
    auto ancestor = node;
    bool ancestorsVisible = true;

    // A subtree capture must also respect hidden ancestors outside that subtree.
    while (ancestor != Node<Widget>{}) {
      auto state = tree.inspect(ancestor);

      if (!state) {
        return std::unexpected(state.error());
      }

      ancestorsVisible = ancestorsVisible && state->visible;
      ancestor = state->parent;
    }

    // Sharing immutable storage keeps snapshot moves allocation-free, including
    // MSVC Debug where moving a vector may allocate an iterator proxy in noexcept.
    auto records = std::make_shared<std::vector<Record>>(0);
    auto collect = [&](auto &self, Node<Widget> current, bool parentVisible) -> std::expected<void, TreeError> {
      auto state = tree.inspect(current);

      if (!state) {
        return std::unexpected(state.error());
      }

      const bool visible = parentVisible && state->visible;

      if (visible && !state->geometry.valid) {
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }

      records->push_back({current.id(), visible ? state->geometry.local.size() : CellSize{}});

      // Hidden descendants still need identity records so all views can prepare
      // against this snapshot after an earlier view invalidates the live tree.
      for (const auto child : state->children) {
        auto result = self(self, child, visible);

        if (!result) {
          return result;
        }
      }

      return {};
    };
    auto captured = collect(collect, node, ancestorsVisible);

    if (!captured) {
      return std::unexpected(captured.error());
    }

    std::ranges::sort(*records, {}, [](const Record &record) {
      return record.id.slot;
    });

    return LayoutSnapshot(node.id().session, std::move(records));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<CellSize, TreeError> LayoutSnapshot::viewport(Node<Widget> node) const noexcept {
  const auto id = node.id();

  if (!id.session || !records_) {
    return std::unexpected(TreeError::STALE_NODE);
  }

  if (id.session != session_) {
    return std::unexpected(TreeError::WRONG_SESSION);
  }

  const auto found = std::ranges::lower_bound(*records_, id.slot, {}, [](const Record &record) {
    return record.id.slot;
  });

  if (found == records_->end() || found->id != id) {
    return std::unexpected(TreeError::STALE_NODE);
  }

  return found->size;
}

std::expected<LayoutSnapshot, TreeError> LayoutSnapshot::capture(WidgetTree &tree, Node<Widget> node,
                                                                 LayoutSnapshotProfile profile) {
  if (profile == LayoutSnapshotProfile::DIMENSIONS) {
    return capture(tree, node);
  }
  if (profile != LayoutSnapshotProfile::GEOMETRY) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  auto revision = tree.geometryRevision();
  if (!revision) {
    return std::unexpected(revision.error());
  }
  const auto inputGeneration = tree.inputGeneration();
  try {
    bool ancestorsVisible = true;
    for (auto ancestor = node; ancestor != Node<Widget>{};) {
      auto value = tree.inspect(ancestor);
      if (!value) {
        return std::unexpected(value.error());
      }
      ancestorsVisible &= value->visible;
      ancestor = value->parent;
    }
    auto records = std::make_shared<std::vector<Record>>(0);
    auto geometry = std::make_shared<std::vector<GeometryRecord>>(0);
    auto collect = [&](auto &self, Node<Widget> current, bool parentVisible) -> std::expected<void, TreeError> {
      auto value = tree.inspect(current);
      if (!value) {
        return std::unexpected(value.error());
      }
      const bool visible = parentVisible && value->visible;
      if (visible && !value->geometry.valid) {
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }
      LayoutGeometrySnapshot captured{value->parent, value->geometry, {}, visible, {}};
      captured.geometryEpoch = value->geometryEpoch;
      captured.parentLayoutEdge = value->parentLayoutEdge;
      if (captured.parentLayoutEdge &&
          (!visible || !captured.geometryEpoch || captured.parentLayoutEdge->parent != value->parent.id() ||
           captured.parentLayoutEdge->child != current.id() ||
           captured.parentLayoutEdge->geometryEpoch != *captured.geometryEpoch)) {
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }
      std::expected<void, TreeError> status;
      auto read = tree.read(current, [&](const Widget &widget) {
        if (visible) {
          auto clip = CellRect::from({}, value->geometry.local.size());
          if (!clip) {
            status = std::unexpected(TreeError::INVALID_GEOMETRY);
            return;
          }
          const auto bounds = *clip;
          if (const auto *layout = dynamic_cast<const Layout *>(&widget)) {
            auto local = layout->childClip(value->geometry.local.size());
            if (!local) {
              status = std::unexpected(local.error());
              return;
            }
            clip = *local;
          }
          if (!bounds.contains(*clip)) {
            status = std::unexpected(TreeError::INVALID_GEOMETRY);
            return;
          }
          auto surface = clip->translated(value->geometry.surface.origin());
          if (!surface) {
            status = std::unexpected(TreeError::INVALID_GEOMETRY);
            return;
          }
          captured.descendantClip = *surface;
        } else {
          captured.geometry.valid = false;
        }
        if (const auto *state = detail::ScrollWidgetAccess::state(widget); state && state->isArea) {
          auto observation = detail::ScrollWidgetAccess::snapshot(widget, current.id(), captured.geometry, visible);
          if (!observation) {
            status = std::unexpected(observation.error());
            return;
          }
          captured.scrollArea = *observation;
        }
      });
      if (!read || !status) {
        return read ? status : read;
      }
      records->push_back({current.id(), visible ? value->geometry.local.size() : CellSize{}});
      geometry->push_back({current.id(), captured});
      for (const auto child : value->children) {
        auto copied = self(self, child, visible);
        if (!copied) {
          return copied;
        }
      }
      return {};
    };
    auto copied = collect(collect, node, ancestorsVisible);
    if (!copied) {
      return std::unexpected(copied.error());
    }
    std::ranges::sort(*records, {}, [](const Record &record) {
      return record.id.slot;
    });
    std::ranges::sort(*geometry, {}, [](const GeometryRecord &record) {
      return record.id.slot;
    });
    auto currentRevision = tree.geometryRevision();
    if (!currentRevision) {
      return std::unexpected(currentRevision.error());
    }
    if (*currentRevision != *revision) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    return LayoutSnapshot(node.id().session, std::move(records), std::move(geometry), *revision, inputGeneration);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<LayoutGeometrySnapshot, TreeError> LayoutSnapshot::geometry(Node<Widget> node) const noexcept {
  const auto id = node.id();
  if (!id.session || !records_) {
    return std::unexpected(TreeError::STALE_NODE);
  }
  if (id.session != session_) {
    return std::unexpected(TreeError::WRONG_SESSION);
  }
  if (!geometry_) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  const auto found = std::ranges::lower_bound(*geometry_, id.slot, {}, [](const GeometryRecord &record) {
    return record.id.slot;
  });
  return found == geometry_->end() || found->id != id
           ? std::expected<LayoutGeometrySnapshot, TreeError>(std::unexpected(TreeError::STALE_NODE))
           : std::expected<LayoutGeometrySnapshot, TreeError>(found->value);
}

std::expected<std::uint64_t, TreeError> LayoutSnapshot::geometryRevision() const noexcept {
  if (!records_) {
    return std::unexpected(TreeError::STALE_NODE);
  }
  return geometry_ ? std::expected<std::uint64_t, TreeError>(geometryRevision_)
                   : std::unexpected(TreeError::INVALID_ARGUMENT);
}

std::expected<detail::ScrollSnapshotAdmission, TreeError> LayoutSnapshot::admitScrollSnapshot(WidgetTree &tree) const {
  if (!geometry_) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  auto root = tree.root();
  if (!root) {
    return std::unexpected(root.error());
  }
  if (root->id().session != session_) {
    return std::unexpected(TreeError::WRONG_SESSION);
  }
  auto allowed = tree.withScope(*root, [](const UiScope &) {
  });
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  const auto current = tree.inputGeneration();
  if (!current || !inputGeneration_) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }
  return current == inputGeneration_ ? detail::ScrollSnapshotAdmission::CURRENT
                                     : detail::ScrollSnapshotAdmission::STALE;
}

struct Application::State {
  struct Run;
  ApplicationOptions options;
  SchedulerClock clock;
  std::thread::id owner = std::this_thread::get_id();
  std::unique_ptr<Scheduler> scheduler;
  std::unique_ptr<WidgetTree> tree;
  std::unique_ptr<EventRouter> router;
  Node<Widget> root;
  std::stop_source stop;
  bool ran{}, operating{}, complete{}, pointerRouting{};
  ApplicationStats stats;
  ApplicationPhase phase{ApplicationPhase::OPEN};
  std::optional<ApplicationError> error;
  ApplicationScene *scene{};
  ApplicationTerminal *legacy{};
  ApplicationReadyTerminal *ready{};
  std::unique_ptr<Run> run;
  UiScope backgroundScope;
  SchedulerConnection<int> backgroundWake;
  std::unique_ptr<BackgroundPool> ownedBackground;

  struct Cancel {
    Application *app;

    void operator()() const noexcept {
      app->requestStop();
    }
  };

  std::unique_ptr<std::stop_callback<Cancel>> cancellation;
  std::stop_token readyStop;

  ~State();

  State(ApplicationOptions policy, SchedulerClock time);

  void observe(ApplicationPhase measuredPhase, std::chrono::nanoseconds elapsed) {
    if (!options.observer.callback) {
      return;
    }

    try {
      options.observer.callback(options.observer.context, measuredPhase, elapsed);
    } catch (...) {
      throw Failure{ApplicationFailure::OBSERVER_FAILED};
    }
  }

  template <class Callback> void measure(ApplicationPhase measuredPhase, Callback &&callback) {
    if (!options.observer.callback) {
      callback();

      return;
    }

    const auto begin = std::chrono::steady_clock::now();

    callback();

    observe(measuredPhase, std::chrono::steady_clock::now() - begin);
  }
};

struct Application::State::Run {
  using Control = std::variant<InputEvent, CellSize>;
  Application &app;
  State &state;
  ApplicationScene &scene;

  // The writer sees only one stable output adapter. Legacy deadline I/O is used
  // exclusively by the synchronous host, while step installs only ready operations.
  struct Output final : FrameOutput {
    State &state;

    explicit Output(State &owner) : state(owner) {
    }

    std::expected<void, TerminalError> submit(std::string bytes) override {
      return state.ready ? state.ready->submit(std::move(bytes)) : state.legacy->submit(std::move(bytes));
    }

    TerminalWrite flush(TerminalDeadline deadline, std::stop_token stop) override {
      return state.ready ? state.ready->flushReady(stop) : state.legacy->flush(deadline, stop);
    }
  } output;

  ApplicationStats &stats;
  ApplicationPhase &phase;
  std::optional<ApplicationError> &error;
  CellSize viewport;
  InputDecoder decoder;
  UiScope scope;
  SchedulerConnection<Control> channel;
  FrameWriter writer;
  std::optional<TerminalRead> batch;
  std::shared_ptr<const Control> pending;
  DiagnosticContext pendingContext;
  std::size_t byteOffset{}, eventOffset{};
  SchedulerTime arrival{}, nextFrame{};
  bool ending{}, finished{}, drain{};
  const std::uint32_t frameInterval;

  Run(Application &application, State &owner)
      : app(application), state(owner), scene(*owner.scene), output(owner), stats(owner.stats), phase(owner.phase),
        error(owner.error), viewport(require(owner.ready ? owner.ready->size() : owner.legacy->size())),
        decoder(require(InputDecoder::create(state.options.input))), scope(require(app.scheduler().makeScope())),
        writer(output, state.options.writer, state.options.scheduler.diagnostics),
        frameInterval((1000u + state.options.framesPerSecond - 1) / state.options.framesPerSecond) {
    require(app.tree().invalidate(app.root(), Invalidation::LAYOUT));
    channel = require(app.scheduler().connect<Control>(
      scope,
      [this](auto control) {
        try {
          if (state.options.observer.callback) {
            state.observe(ApplicationPhase::CONTROL_LATENCY,
                          std::chrono::milliseconds(app.scheduler().metrics().control.last_latency));
          }

          if (const auto *size = std::get_if<CellSize>(control.get())) {
            require(app.router().cancelPointerCapture());
            viewport = *size;
            require(app.tree().invalidate(app.root(), Invalidation::LAYOUT));

            return;
          }

          const auto &event = std::get<InputEvent>(*control);

          ++stats.inputs;

          if (state.pointerRouting) {
            if (const auto *key = std::get_if<KeyEvent>(&event);
                key && key->key == InputKey::ESCAPE && key->action != KeyAction::RELEASE) {
              require(app.router().cancelPointerCapture());
            }

            if (const auto *mouse = std::get_if<MouseInputEvent>(&event);
                mouse && (mouse->action == MouseAction::MOVE ||
                          (mouse->action == MouseAction::RELEASE && mouse->button == MouseButton::LEFT))) {
              const auto captured = require(app.router().pointerCapture());

              if (captured != Node<Widget>{}) {
                // A global interceptor must not swallow a captured continuation or release.
                require(app.router().dispatch(event));

                return;
              }
            }
          }

          if (!require(scene.input(app, event))) {
            require(app.router().dispatch(event));
          }
        } catch (...) {
          phase = ApplicationPhase::INPUT;
          captureFailure(phase, error);
          app.requestStop();
        }
      },
      SchedulerLane::CONTROL));
  }

  void readInput() {
    if (!batch && !pending && !drain && !ending) {
      arrival = app.now();

      auto deadline = deadlineAt(arrival) + state.options.inputSlice;

      if (const auto decodeDeadline = decoder.deadline()) {
        if (*decodeDeadline <= arrival) {
          drain = true;

          return;
        }

        deadline = std::min(deadline, deadlineAt(*decodeDeadline));
      }

      // Only the synchronous host reaches the legacy branch. The public step
      // accepts no deadline transport and therefore cannot accidentally read(now).
      batch.emplace(state.ready ? state.ready->poll(state.stop.get_token())
                                : state.legacy->read(deadline, state.stop.get_token()));
      arrival = app.now();
      byteOffset = eventOffset = 0;

      if (batch->error) {
        throw Failure{*batch->error};
      }

      if (batch->status == TerminalStatus::STOPPED) {
        app.requestStop();

        return;
      }

      if (batch->status == TerminalStatus::FAILED) {
        throw Failure{TerminalError{TerminalErrorKind::SYSTEM}};
      }

      ending = batch->status == TerminalStatus::END_OF_INPUT;
    }
  }

  void admitInput() {
    // Keep exactly one native batch/raw suffix and one rejected CONTROL owner. A full
    // queue never consumes the retained event; dispatch below releases queue capacity.

    for (std::size_t work = 0; work < state.options.inputBudget; ++work) {
      if (pending) {
        DiagnosticScope diagnostic(app.tree().diagnostics(), pendingContext);

        auto published = channel.publisher.publish(pending);

        if (!published && published.error() == SchedulerError::WOULD_BLOCK) {
          break;
        }

        require(std::move(published));
        pending.reset();
      }

      std::optional<Control> control;

      if (batch && batch->resized) {
        control.emplace(*batch->resized);
        batch->resized.reset();
      } else if (batch && eventOffset < batch->events.size()) {
        control.emplace(batch->events[eventOffset++]);
      } else {
        if (finished) {
          break;
        }

        const auto bytes = batch ? std::string_view(batch->bytes).substr(byteOffset) : std::string_view{};
        const auto stamp = bytes.empty() ? app.now() : arrival;
        auto step = require(ending && bytes.empty() ? decoder.finish(stamp) : decoder.next(bytes, stamp));

        byteOffset += step.consumed;
        drain = step.status == InputStatus::WORK_LIMIT || step.event.has_value();
        finished = step.status == InputStatus::FINISHED;

        if (step.status == InputStatus::FAILED) {
          throw Failure{InputError::FAILED};
        }

        if (step.event) {
          control.emplace(std::move(*step.event));
        }

        if (batch && byteOffset == batch->bytes.size() && eventOffset == batch->events.size()) {
          batch.reset();
        }

        if (!control && !drain) {
          break;
        }
      }

      if (control) {
        pendingContext = {};

        if (auto *diagnostics = app.tree().diagnostics()) {
          const auto *event = std::get_if<InputEvent>(&*control);
          pendingContext = diagnostics->beginInput(event ? inputCategory(*event) : DiagnosticInput::OTHER);
        }

        pending = std::make_shared<const Control>(std::move(*control));
      }
    }
  }

  bool dispatch() {
    phase = ApplicationPhase::DISPATCH;
    const auto dispatched = require(app.scheduler().dispatch());

    if (dispatched.callback_failures) {
      throw Failure{SchedulerError::CALLBACK_FAILED};
    }

    if (!dispatched.stopped && !state.stop.stop_requested()) {
      if (auto *executor = app.background()) {
        executor->service();
      }

      require(scene.afterDispatch(app));
    }

    return !dispatched.stopped && !state.stop.stop_requested();
  }

  CellBuffer buildFrame() {
    // Commands borrow widget text. Consume them into owning cells before releasing
    // the tree frame; keep original component errors across its callback boundary.
    std::optional<CellBuffer> frame;
    std::optional<Failure> failure;

    auto framed = app.tree().frame([&] {
      try {
        const auto rect = require(CellRect::from({}, viewport).transform_error([](auto) {
          return TreeError::INVALID_GEOMETRY;
        }));
        std::vector<RenderCommand> commands(std::initializer_list<RenderCommand>{});

        require(layoutTree(app.tree(), app.root(), rect, rect));
        paintTree(app.tree(), app.root(), commands, state.options.render.max_commands);
        frame.emplace(require(renderFullFrame(viewport, commands, state.options.widthPolicy, state.options.render)));
      } catch (const Failure &value) {
        failure.emplace(value);
      }
    });

    if (failure) {
      throw *failure;
    }

    require(std::move(framed));

    return std::move(*frame);
  }

  LayoutSnapshot captureLayout() {
    std::optional<LayoutSnapshot> snapshot;
    std::optional<Failure> failure;
    auto framed = app.tree().frame([&] {
      try {
        const auto rect = require(CellRect::from({}, viewport).transform_error([](auto) {
          return TreeError::INVALID_GEOMETRY;
        }));

        require(layoutTree(app.tree(), app.root(), rect, rect));
        snapshot.emplace(require(scene.usesGeometrySnapshot()
                                   ? LayoutSnapshot::capture(app.tree(), app.root(), LayoutSnapshotProfile::GEOMETRY)
                                   : LayoutSnapshot::capture(app.tree(), app.root())));
      } catch (const Failure &value) {
        failure.emplace(value);
      }
    });

    if (failure) {
      throw *failure;
    }

    require(std::move(framed));

    return std::move(*snapshot);
  }

  Invalidation consumePreparationRequest(DiagnosticContext &frameContext) {
    const auto dirty = require(app.tree().consumeFrameRequest());
    const auto requestedContext = app.tree().diagnosticFrame();

    // Layout's in-frame PAINT has no new causal token. Keep the last real request
    // so an otherwise unchanged preparation still links input to submitted output.
    if (requestedContext.frameId) {
      frameContext = requestedContext;
    }

    return dirty;
  }

  void prepareViews(DiagnosticContext &frameContext) {
    for (std::size_t pass = 0; pass < state.options.maxPreparationPasses; ++pass) {
      const auto snapshot = captureLayout();

      // Arrangement requests PAINT. Only fresh LAYOUT work from view preparation
      // repeats the pass; all hooks share the pin established by prepare once.
      consumePreparationRequest(frameContext);
      require(scene.prepareViews(app, snapshot));

      if (consumePreparationRequest(frameContext) != Invalidation::LAYOUT) {
        return;
      }
    }

    throw Failure{TreeError::WOULD_BLOCK};
  }

  void renderReady() {
    if (writer.pending() || app.now() < nextFrame) {
      return;
    }

    const auto dirty = require(app.tree().consumeFrameRequest());

    if (dirty == Invalidation::NONE) {
      return;
    }

    auto frameContext = app.tree().diagnosticFrame();
    DiagnosticScope preparing(app.tree().diagnostics(), frameContext);

    phase = ApplicationPhase::PREPARE;
    state.measure(phase, [&] {
      require(scene.prepare(app, viewport));
      consumePreparationRequest(frameContext);

      if (scene.usesPreparedLayout()) {
        prepareViews(frameContext);
      }
    });

    std::optional<CellBuffer> frame;

    phase = ApplicationPhase::RENDER;
    state.measure(phase, [&] {
      frame.emplace(buildFrame());
    });

    // arrange requests paint inside layout, but this frame has already painted it.
    // Consume that work without losing the actual submission's causal token.
    require(app.tree().consumeFrameRequest());

    require(scene.completedLayout(app));

    phase = ApplicationPhase::SUBMIT;
    state.measure(phase, [&] {
      require(writer.submit(std::move(*frame), frameContext));
    });
    ++stats.frames;
    nextFrame = app.now() + frameInterval;
  }

  void flushOutput() {
    if (writer.pending()) {
      phase = ApplicationPhase::FLUSH;
      state.measure(phase, [&] {
        const auto written = writer.flush(deadlineAt(app.now()) + state.options.outputSlice, state.stop.get_token());

        ++stats.flushes;
        stats.bytes += written.completed;

        if (written.error) {
          throw Failure{*written.error};
        }

        if (written.status == TerminalStatus::STOPPED) {
          app.requestStop();
        }

        if (written.status == TerminalStatus::FAILED) {
          throw Failure{TerminalError{TerminalErrorKind::SYSTEM}};
        }

        if (!writer.pending()) {
          nextFrame = app.now() + frameInterval;
        }
      });
    }
  }

  bool process() {
    if (state.stop.stop_requested()) {
      return false;
    }

    admitInput();

    if (!dispatch()) {
      return false;
    }

    renderReady();

    return true;
  }

  bool finishedWork() const {
    return state.stop.stop_requested() ||
           (finished && !pending && !batch && app.scheduler().metrics().control.pending == 0 &&
            !scene.hasPendingLayout() && !writer.pending());
  }
};

std::expected<void, TreeError> ApplicationScene::prepare(Application &, CellSize) {
  return {};
}

bool ApplicationScene::usesPreparedLayout() const noexcept {
  return false;
}

bool ApplicationScene::usesGeometrySnapshot() const noexcept {
  return false;
}

bool ApplicationScene::requiresPointerMotion() const noexcept {
  return false;
}

PointerCaptureCapabilities ApplicationTerminal::pointerCaptureCapabilities() const noexcept {
  return {};
}

PointerCaptureCapabilities ApplicationReadyTerminal::pointerCaptureCapabilities() const noexcept {
  return {};
}

std::expected<void, TreeError> ApplicationScene::prepareViews(Application &, const LayoutSnapshot &) {
  return {};
}

std::expected<void, TreeError> ApplicationScene::completedLayout(Application &) {
  return {};
}

std::expected<void, TreeError> ApplicationScene::afterDispatch(Application &) {
  return {};
}

bool ApplicationScene::requiresBackgroundExecutor() const noexcept {
  return false;
}

bool ApplicationScene::hasPendingLayout() const noexcept {
  return false;
}

std::expected<bool, TreeError> ApplicationScene::input(Application &, const InputEvent &) {
  return false;
}

void ApplicationScene::stop(Application &) {
}

Application::Application(std::unique_ptr<State> state) : state_(std::move(state)) {
}

Application::State::State(ApplicationOptions policy, SchedulerClock time) : options(std::move(policy)), clock(time) {
}

Application::State::~State() = default;

BackgroundExecutor *Application::background() noexcept {
  return state_->options.backgroundExecutor ? state_->options.backgroundExecutor : state_->ownedBackground.get();
}

void Application::prepareBackground(const ApplicationScene &scene, bool hosted) {
  if (!scene.requiresBackgroundExecutor() || background()) {
    return;
  }

  if (hosted) {
    throw Failure{ApplicationFailure::BACKGROUND_FAILED, BackgroundError::UNAVAILABLE};
  }

  state_->backgroundScope = require(scheduler().makeScope());
  state_->backgroundWake = require(scheduler().connect<int>(state_->backgroundScope, [](auto) {
  }));
  state_->ownedBackground =
    require(BackgroundPool::create(state_->options.background, state_->backgroundWake.publisher));
}

void Application::finishBackground() noexcept {
  if (state_->ownedBackground) {
    // This is the synchronous run's final boundary, after finishSession has
    // attempted terminal restoration. No UI acknowledgement is needed to join.
    state_->ownedBackground->join();
    state_->ownedBackground.reset();
  }

  state_->backgroundWake = {};
  state_->backgroundScope = {};
}

Application::~Application() {
  if (state_->scene && !state_->complete) {
    state_->operating = true;
    finishSession();
  }
}

Scheduler &Application::scheduler() noexcept {
  return *state_->scheduler;
}

WidgetTree &Application::tree() noexcept {
  return *state_->tree;
}

EventRouter &Application::router() noexcept {
  return *state_->router;
}

Node<Widget> Application::root() const noexcept {
  return state_->root;
}

WidthPolicy Application::widthPolicy() const noexcept {
  return state_->options.widthPolicy;
}

SchedulerTime Application::now() const noexcept {
  if (state_->clock.read) {
    return state_->clock.read(state_->clock.context);
  }

  return static_cast<SchedulerTime>(
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Application::requestStop() noexcept {
  state_->stop.request_stop();
  state_->scheduler->requestStop();
}

std::expected<std::unique_ptr<Application>, ApplicationError> Application::create(ApplicationOptions options,
                                                                                  SchedulerClock clock) {
  if (!options.framesPerSecond || options.framesPerSecond > 1000 || !options.inputBudget ||
      !options.maxPreparationPasses || options.inputSlice.count() <= 0 || options.outputSlice.count() <= 0 ||
      (options.nativeScreen != NativeScreenMode::MAIN_SCREEN &&
       options.nativeScreen != NativeScreenMode::ALTERNATE_SCREEN)) {
    return std::unexpected(ApplicationError{ApplicationPhase::CREATE, ApplicationFailure::INVALID_OPTIONS, {}});
  }

  try {
    auto state = std::make_unique<State>(std::move(options), clock);

    state->scheduler = require(Scheduler::create(state->options.scheduler, clock));
    state->tree = require(WidgetTree::create(*state->scheduler));
    state->root = require(state->tree->root());
    state->router = require(EventRouter::create(*state->tree));
    require(InputDecoder::create(state->options.input));

    return std::unique_ptr<Application>(new Application(std::move(state)));
  } catch (const Failure &error) {
    return std::unexpected(ApplicationError{ApplicationPhase::CREATE, error.cause, {}});
  } catch (const std::bad_alloc &) {
    return std::unexpected(ApplicationError{ApplicationPhase::CREATE, ApplicationFailure::NO_MEMORY, {}});
  }
}

std::expected<ApplicationStats, ApplicationError> Application::run(ApplicationScene &scene, std::stop_token stop) {
  if (state_->owner != std::this_thread::get_id()) {
    return std::unexpected(ApplicationError{ApplicationPhase::OPEN, TreeError::WRONG_THREAD, {}});
  }

  if (state_->clock.read) {
    return std::unexpected(ApplicationError{ApplicationPhase::OPEN, ApplicationFailure::INVALID_OPTIONS, {}});
  }

  if (state_->ran) {
    return std::unexpected(ApplicationError{ApplicationPhase::OPEN, ApplicationFailure::ALREADY_RUN, {}});
  }

  try {
    prepareBackground(scene, false);
#ifdef _WIN32
    NativeTerminalHandles handles{reinterpret_cast<std::intptr_t>(GetStdHandle(STD_INPUT_HANDLE)),
                                  reinterpret_cast<std::intptr_t>(GetStdHandle(STD_OUTPUT_HANDLE))};
#else
    NativeTerminalHandles handles{0, 1};
#endif
    const auto buttonMotion = scene.requiresPointerMotion();
    auto opened =
      openNativeApplicationTerminal(handles, state_->options.terminal, state_->options.outputSlice,
                                    state_->options.nativeMouseReporting, state_->options.nativeScreen, buttonMotion);

    if (!opened) {
      state_->ran = true;
      finishBackground();

      return std::unexpected(ApplicationError{ApplicationPhase::OPEN, opened.error(), {}});
    }

    return run(scene, **opened, stop);
  } catch (...) {
    state_->ran = true;
    captureFailure(ApplicationPhase::OPEN, state_->error);
    finishBackground();

    return std::unexpected(*state_->error);
  }
}

std::expected<void, ApplicationError> Application::start(ApplicationScene &scene, ApplicationReadyTerminal &terminal,
                                                         std::stop_token externalStop) {
  if (state_->owner != std::this_thread::get_id()) {
    return std::unexpected(ApplicationError{ApplicationPhase::OPEN, TreeError::WRONG_THREAD, {}});
  }

  if (state_->operating) {
    return std::unexpected(ApplicationError{ApplicationPhase::OPEN, ApplicationFailure::REENTRANT_OPERATION, {}});
  }

  if (state_->ran) {
    return std::unexpected(ApplicationError{ApplicationPhase::OPEN, ApplicationFailure::ALREADY_RUN, {}});
  }

  state_->ran = state_->operating = true;
  state_->scene = &scene;
  state_->ready = &terminal;

  try {
    prepareBackground(scene, true);
    // The ready host never waits: sampling its retained token avoids destroying
    // a stop_callback that could join a concurrently executing external callback.
    state_->readyStop = externalStop;

    const auto capabilities = terminal.pointerCaptureCapabilities();

    require(router().setPointerCaptureCapabilities(capabilities));
    state_->pointerRouting = capabilities.motion && capabilities.release;

    if (state_->readyStop.stop_requested()) {
      requestStop();
    }

    if (!state_->stop.stop_requested()) {
      state_->run = std::make_unique<State::Run>(*this, *state_);
    } else {
      finishSession();
    }
  } catch (...) {
    captureFailure(state_->phase, state_->error);
    finishSession();
  }

  state_->operating = false;

  if (state_->error) {
    return std::unexpected(*state_->error);
  }

  return {};
}

std::expected<ApplicationStep, ApplicationError> Application::step() {
  if (state_->owner != std::this_thread::get_id()) {
    return std::unexpected(ApplicationError{ApplicationPhase::OPEN, TreeError::WRONG_THREAD, {}});
  }

  if (state_->operating) {
    return std::unexpected(ApplicationError{ApplicationPhase::OPEN, ApplicationFailure::REENTRANT_OPERATION, {}});
  }

  if (!state_->ran || (!state_->ready && !state_->complete)) {
    return std::unexpected(ApplicationError{ApplicationPhase::OPEN, ApplicationFailure::NOT_STARTED, {}});
  }

  state_->operating = true;

  if (state_->readyStop.stop_requested()) {
    requestStop();
  }

  if (!state_->complete && state_->stop.stop_requested()) {
    finishSession();
  }

  if (!state_->complete) {
    try {
      ++state_->stats.iterations;
      state_->phase = ApplicationPhase::INPUT;
      state_->run->readInput();

      if (state_->run->process()) {
        state_->run->flushOutput();
      }

      if (state_->run->finishedWork()) {
        finishSession();
      }
    } catch (...) {
      captureFailure(state_->phase, state_->error);
      finishSession();
    }
  }

  state_->operating = false;

  if (state_->error) {
    return std::unexpected(*state_->error);
  }

  return ApplicationStep{state_->complete, state_->stats};
}

ApplicationStats Application::stats() const noexcept {
  return state_->stats;
}

void Application::finishSession() noexcept {
  if (state_->complete) {
    return;
  }

  if (state_->ownedBackground) {
    state_->ownedBackground->requestStop();
  }

  // Disconnect callbacks that capture Run before stopping producers or the scene.
  // All progress and errors remain in State after the engine storage is released.
  state_->run.reset();
  state_->complete = true;
  state_->phase = ApplicationPhase::STOP;

  try {
    require(router().cancelPointerCapture(false));
    require(router().setPointerCaptureCapabilities({}));
  } catch (...) {
    captureFailure(state_->phase, state_->error);
  }

  try {
    state_->measure(state_->phase, [&] {
      requestStop();
      require(scheduler().stop());
      state_->scene->stop(*this);
    });
  } catch (...) {
    captureFailure(state_->phase, state_->error);
  }

  state_->phase = ApplicationPhase::CLOSE;
  std::optional<TerminalError> restoration;

  try {
    state_->measure(state_->phase, [&] {
      auto closed = state_->ready ? state_->ready->close() : state_->legacy->close();

      if (!closed) {
        restoration = closed.error();
      }
    });
  } catch (...) {
    captureFailure(state_->phase, state_->error);
  }

  if (restoration) {
    if (!state_->error) {
      state_->error.emplace(ApplicationError{state_->phase, *restoration, {}});
    }

    state_->error->restoration = restoration;
  }

  state_->cancellation.reset();
  state_->readyStop = {};
  state_->scene = nullptr;
  state_->legacy = nullptr;
  state_->ready = nullptr;
}

std::expected<ApplicationStats, ApplicationError>
Application::run(ApplicationScene &scene, ApplicationTerminal &terminal, std::stop_token externalStop) {
  if (state_->owner != std::this_thread::get_id()) {
    return std::unexpected(ApplicationError{ApplicationPhase::OPEN, TreeError::WRONG_THREAD, {}});
  }

  if (state_->ran) {
    return std::unexpected(ApplicationError{ApplicationPhase::OPEN, ApplicationFailure::ALREADY_RUN, {}});
  }

  state_->ran = state_->operating = true;
  state_->scene = &scene;
  state_->legacy = &terminal;

  try {
    prepareBackground(scene, false);
    const auto capabilities = terminal.pointerCaptureCapabilities();

    require(router().setPointerCaptureCapabilities(capabilities));
    state_->pointerRouting = capabilities.motion && capabilities.release;
    state_->cancellation = std::make_unique<std::stop_callback<State::Cancel>>(externalStop, State::Cancel{this});

    if (!state_->stop.stop_requested()) {
      state_->run = std::make_unique<State::Run>(*this, *state_);

      while (!state_->stop.stop_requested()) {
        ++state_->stats.iterations;
        state_->phase = ApplicationPhase::INPUT;
        // Deadline reads and flushes belong to this synchronous host, outside
        // the shared bounded processing engine and the public ready-only step.
        state_->run->readInput();

        if (state_->run->process()) {
          state_->run->flushOutput();
        }

        if (state_->run->finishedWork()) {
          break;
        }
      }
    }
  } catch (...) {
    captureFailure(state_->phase, state_->error);
  }

  finishSession();
  finishBackground();
  state_->operating = false;

  if (state_->error) {
    return std::unexpected(*state_->error);
  }

  return state_->stats;
}
} // namespace portals
