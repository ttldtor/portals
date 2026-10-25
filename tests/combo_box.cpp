// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "support/choice_frame.hpp"
#include <doctest.h>
#include <portals/ui.hpp>

#include <chrono>
#include <format>
#include <thread>

namespace {
using namespace portals;
using namespace std::chrono_literals;

template <class T, class E>
T take(std::expected<T, E> value, std::source_location where = std::source_location::current()) {
  if (!value) {
    std::cerr << where.file_name() << ':' << where.line() << " fixture failure";
    if constexpr (std::same_as<E, TreeError>) {
      std::cerr << " TreeError=" << static_cast<int>(value.error());
    }
    if constexpr (std::same_as<E, ManagedError>) {
      if (auto cause = std::get_if<TreeError>(&value.error().cause)) {
        std::cerr << " TreeError=" << static_cast<int>(*cause);
      }
      std::cerr << " phase=" << static_cast<int>(value.error().phase);
    }
    std::cerr << '\n';
    throw std::runtime_error("choice fixture operation failed");
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Probe {
  std::atomic<std::size_t> reads{}, rows{}, projections{};
  const std::thread::id owner = std::this_thread::get_id();
  std::atomic<bool> workerOnly{true};
  std::atomic<bool> throwProjection{};
};

class Rows final : public ModelSnapshot<std::string> {
public:
  Rows(std::shared_ptr<Probe> probe, std::size_t count = 80, std::uint64_t revision = 1)
      : ModelSnapshot(revision, count), probe_(std::move(probe)) {
    for (std::size_t index = 0; index < count; ++index) {
      rows_.push_back({{index + 1, 1}, index == 1 || index == 65 ? "Duplicate" : "City " + std::to_string(index)});
    }
  }

  ModelRows<std::string> read(RowRange range) const override {
    ++probe_->reads;
    probe_->rows += range.count;
    return {RangeState::READY, std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

private:
  std::shared_ptr<Probe> probe_;
  std::vector<ModelRow<std::string>> rows_;
};

struct Fixture {
  std::unique_ptr<BackgroundPool> pool = take(BackgroundPool::create());
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;
  std::shared_ptr<Probe> probe = std::make_shared<Probe>();
  ManagedModel<std::string> model;
  ui::Ref<ComboBox> choice;
  ui::Ref<DropDown> readOnly;
  std::vector<ChoiceSelection> notices;
  std::vector<SearchError> errors;
  Node<Column> body;

  explicit Fixture(std::size_t count = 80, ui::ComboBoxConfig config = {}, ManagedOptions options = {},
                   std::shared_ptr<const ModelSnapshot<std::string>> initial = {}, bool formatted = false,
                   BackgroundExecutor *executor = nullptr) {
    app = take(Application::create(ApplicationOptions{.backgroundExecutor = executor ? executor : pool.get()}));
    scene = take(ManagedScene::create(*app, options));
    model = take(scene->model<std::string>(initial ? std::move(initial) : std::make_shared<const Rows>(probe, count)));
    config.failed = [this, previous = std::move(config.failed)](SearchError error) {
      errors.push_back(error);
      if (previous) {
        previous(error);
      }
    };
    auto projection = [probe = probe](const std::string &text) -> std::string_view {
      ++probe->projections;
      if (probe->owner == std::this_thread::get_id()) {
        probe->workerOnly = false;
      }
      if (probe->throwProjection) {
        throw std::runtime_error("projection failure");
      }
      return text;
    };
    auto recipe = formatted ? ui::comboBox(
                                choice, model,
                                [projection](const std::string &text) {
                                  return std::format("Place: {}", projection(text));
                                },
                                std::move(config))
                            : ui::comboBox(choice, model, std::move(projection), std::move(config));
    body = take(ui::mount(*scene, ui::vbox(std::move(recipe), ui::dropDown(readOnly, {"Courier", "Pickup", "Post"}))));
    take(ui::detail::choiceSelectHandler(
      ui::detail::NamedAccess::target(choice),
      [this](ChoiceSelection value) {
        notices.push_back(std::move(value));
      },
      std::source_location::current()));
  }

  ~Fixture() {
    scene.reset();
    app.reset();
    pool->requestStop();
    pool->join();
  }

  void pump() {
    take(app->scheduler().dispatch());
    app->background()->service();
    take(scene->afterDispatch(*app));
    take(scene->prepare(*app, {80, 20}));
    const auto bounds = take(CellRect::from({}, {80, 20}));
    for (unsigned pass = 0; pass < 4; ++pass) {
      take(app->tree().consumeFrameRequest());
      std::optional<LayoutSnapshot> layout;
      take(app->tree().frame([&] {
        take(layoutTree(app->tree(), app->root(), bounds, bounds));
        layout.emplace(take(LayoutSnapshot::capture(app->tree(), app->root())));
      }));
      take(scene->prepareViews(*app, *layout));
      if (take(app->tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
        break;
      }
    }
    take(scene->completedLayout(*app));
    app->background()->service();
  }

  void input(InputEvent event) {
    const auto handled = take(scene->input(*app, event));
    if (!handled) {
      take(app->router().dispatch(event));
    }
    pump();
  }

  template <class Predicate> bool until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
      pump();
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    for (const auto &diagnostic : take(scene->diagnostics()).errors) {
      std::cerr << "managed failure operation=" << (diagnostic.error.operation ? diagnostic.error.operation : "?");
      if (auto cause = std::get_if<TreeError>(&diagnostic.error.cause)) {
        std::cerr << " TreeError=" << static_cast<int>(*cause);
      }
      std::cerr << " phase=" << static_cast<int>(diagnostic.error.phase) << '\n';
    }
    std::cerr << "reads=" << probe->reads << " projected=" << probe->projections << "\n";
    std::vector<Node<Widget>> nodes{scene->root()};
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      auto snapshot = take(app->tree().inspect(nodes[index]));
      take(app->tree().read(nodes[index], [&](const Widget &widget) {
        std::cerr << typeid(widget).name() << " visible=" << snapshot.visible
                  << " geometry=" << snapshot.geometry.surface.size().width << 'x'
                  << snapshot.geometry.surface.size().height << " valid=" << snapshot.geometry.valid;
        if (const auto *list = dynamic_cast<const VirtualList<SearchItem> *>(&widget)) {
          std::cerr << " state=" << static_cast<int>(list->rangeState()) << " count=" << list->dataStatus().rowCount
                    << " range=" << list->visibleRange().first << ',' << list->visibleRange().count;
        }
        std::cerr << '\n';
      }));
      nodes.insert(nodes.end(), snapshot.children.begin(), snapshot.children.end());
    }
    return predicate();
  }

  Node<Dialog> popup() {
    Node<Dialog> result;
    take(app->tree().read(choice.node(), [&](const ComboBox &widget) {
      result = widget.popup();
    }));
    return result;
  }

  Node<VirtualList<SearchItem>> browse() {
    return popupList(1);
  }

  Node<VirtualList<SearchItem>> popupList(std::size_t index) {
    auto popupChildren = take(app->tree().inspect(popup())).children;
    auto panelChildren = take(app->tree().inspect(popupChildren.front())).children;
    auto bodyChildren = take(app->tree().inspect(panelChildren.front())).children;
    ui::Ref<VirtualList<SearchItem>> result;
    take(ui::detail::NamedAccess::publish(result, *scene, bodyChildren[index]));
    return result.node();
  }
};

TEST_CASE("closed model choices read no rows and keep draft separate from selection") {
  Fixture f;
  for (int cycle = 0; cycle < 5; ++cycle) {
    f.pump();
  }
  CHECK(f.probe->reads == 0);
  CHECK(f.probe->projections == 0);
  CHECK_FALSE(take(f.choice.selection()));
  take(f.choice.text().set("Independent draft").result());
  f.pump();
  CHECK(take(f.choice.text().get()) == "Independent draft");
  CHECK_FALSE(take(f.choice.selection()));
  CHECK(f.notices.empty());
}

TEST_CASE("programmatic ordinal selection resolves only one worker row and clear notifies once") {
  Fixture f;
  f.pump();
  take(f.choice.select(65));
  CHECK_FALSE(take(f.choice.selection()));
  REQUIRE(f.until([&] {
    return bool(take(f.choice.selection()));
  }));
  auto selected = take(f.choice.selection());
  CHECK(selected.index() == 65);
  CHECK(selected.rowId() == RowId{66, 1});
  CHECK(selected.text() == "Duplicate");
  CHECK(f.probe->rows == 1);
  CHECK(f.probe->workerOnly);
  CHECK(f.notices.size() == 1);
  take(f.choice.select(selected));
  CHECK(f.notices.size() == 1);
  take(f.choice.clearSelection());
  REQUIRE(f.notices.size() == 2);
  CHECK_FALSE(f.notices.back());
  take(f.choice.clearSelection());
  CHECK(f.notices.size() == 2);
  CHECK(take(f.choice.text().get()) == "Duplicate");
  take(f.scene->close());
  CHECK(selected.text() == "Duplicate");
}

TEST_CASE("saved selections admit inside button callbacks without changing draft during dispatch") {
  Fixture f;
  f.pump();
  take(f.choice.select(65));
  REQUIRE(f.until([&] {
    return bool(take(f.choice.selection()));
  }));
  auto saved = take(f.choice.selection());
  take(f.choice.clearSelection());
  take(f.choice.text().set("Keep draft until delivery").result());
  auto button = take(ui::detail::mountAt(*f.scene, f.body,
                                         ui::button("Restore",
                                                    [choice = f.choice, saved] {
                                                      return choice.select(saved);
                                                    }),
                                         ui::ControlContext{}));
  take(f.app->router().invoke(button, {portals::Action::ACTIVATE}));
  CHECK(take(f.choice.text().get()) == "Keep draft until delivery");
  CHECK_FALSE(take(f.choice.selection()));
  REQUIRE(f.until([&] {
    return bool(take(f.choice.selection()));
  }));
  CHECK(take(f.choice.text().get()) == "Duplicate");
  CHECK(take(f.scene->diagnostics()).errors.empty());

  take(f.choice.clearSelection());
  take(f.choice.select(saved));
  take(f.choice.text().set("Newer draft").result());
  f.pump();
  CHECK_FALSE(take(f.choice.selection()));
  CHECK(take(f.choice.text().get()) == "Newer draft");
  take(f.choice.select(saved));
  take(f.choice.clearSelection());
  f.pump();
  CHECK_FALSE(take(f.choice.selection()));
}

TEST_CASE("unfiltered popup has full source ordinal space with bounded worker ranges") {
  Fixture f;
  f.pump();
  take(f.choice.open());
  REQUIRE(f.until([&] {
    return take(f.choice.opened());
  }));
  auto list = f.browse();
  REQUIRE(f.until([&] {
    bool ready{};
    take(f.app->tree().read(list, [&](const auto &view) {
      ready = view.rangeState() == RangeState::READY;
    }));
    return ready;
  }));
  CHECK(f.probe->rows <= 12);
  take(setVirtualListFirst(f.app->tree(), list, 64));
  REQUIRE(f.until([&] {
    bool ready{};
    take(f.app->tree().read(list, [&](const auto &view) {
      ready = view.rangeState() == RangeState::READY && view.visibleRange().first == 64;
    }));
    return ready;
  }));
  CHECK(f.probe->rows <= 26);
  take(setVirtualListSelection(f.app->tree(), list, RowId{66, 1}));
  take(f.app->router().invoke(list, {portals::Action::ACTIVATE}));
  REQUIRE(f.until([&] {
    return bool(take(f.choice.selection()));
  }));
  CHECK(take(f.choice.selection()).index() == 65);
  CHECK_FALSE(take(f.choice.opened()));
  CHECK(f.errors.empty());
}

TEST_CASE("scene-level onSelect uses standard callback adaptation and explicit host is reused") {
  ui::Ref<ComboBox> ref;
  auto scene = ui::scene(ui::overlayHost(ui::comboBox(ref, {"One", "Two"})));
  scene | ui::onSelect(ref, []([[maybe_unused]] ChoiceSelection value) {
  });
  take(scene.prepareChecked());
  CHECK(ref.node() != Node<ComboBox>{});
  CHECK_FALSE(take(ref.selection()));
}

TEST_CASE("source replacement clears accepted selection once and preserves the visible draft") {
  Fixture f;
  f.pump();
  take(f.choice.select(1));
  REQUIRE(f.until([&] {
    return bool(take(f.choice.selection()));
  }));
  auto saved = take(f.choice.selection());
  auto publisher = take(f.model.publisher());
  take(publisher.publish(std::make_shared<const Rows>(f.probe, 80, 2)));
  REQUIRE(f.until([&] {
    return !take(f.choice.selection());
  }));
  REQUIRE(f.notices.size() == 2);
  CHECK_FALSE(f.notices.back());
  CHECK(take(f.choice.text().get()) == "Duplicate");
  CHECK_FALSE(f.choice.select(saved));
  CHECK(saved.text() == "Duplicate");
}

TEST_CASE("a source replacement fences a running old one-row selection") {
  struct Gate {
    std::atomic<bool> entered{}, released{};
  };

  class DelayedRows final : public ModelSnapshot<std::string> {
  public:
    explicit DelayedRows(std::shared_ptr<Gate> gate) : ModelSnapshot(1, 1), gate_(std::move(gate)) {
    }

    ModelRows<std::string> read(RowRange) const override {
      gate_->entered = true;
      while (!gate_->released) {
        std::this_thread::sleep_for(1ms);
      }
      return {RangeState::READY, rows_, {}};
    }

  private:
    std::shared_ptr<Gate> gate_;
    std::array<ModelRow<std::string>, 1> rows_{{{{1, 1}, "Old row"}}};
  };

  const auto gate = std::make_shared<Gate>();
  Fixture f(0, {}, {}, std::make_shared<const DelayedRows>(gate));

  struct Release {
    std::shared_ptr<Gate> gate;

    ~Release() {
      gate->released = true;
    }
  } release{gate};

  f.pump();
  take(f.choice.select(0));
  REQUIRE(f.until([&] {
    return gate->entered.load();
  }));
  auto publisher = take(f.model.publisher());
  take(publisher.publish(std::make_shared<const Rows>(f.probe, 80, 2)));
  f.pump();
  take(f.choice.select(65));
  gate->released = true;
  REQUIRE(f.until([&] {
    return bool(take(f.choice.selection()));
  }));
  CHECK(take(f.choice.selection()).index() == 65);
  CHECK(take(f.choice.selection()).text() == "Duplicate");
  CHECK(f.notices.size() == 1);
  CHECK(take(f.scene->diagnostics()).errors.empty());
}

TEST_CASE("terminal worker admission failures retire selection intent before capacity recovery") {
  struct GateExecutor final : BackgroundExecutor {
    std::unique_ptr<BackgroundPool> pool = take(BackgroundPool::create());
    std::optional<BackgroundError> rejected;
    bool full{};
    std::uint64_t epoch{};
    std::size_t submissions{};

    std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &job) override {
      ++submissions;
      if (full) {
        return std::unexpected(BackgroundError::WOULD_BLOCK);
      }
      if (rejected) {
        return std::unexpected(*rejected);
      }
      return pool->submit(job);
    }

    void service() noexcept override {
      pool->service();
    }

    std::uint64_t capacityEpoch() const noexcept override {
      return epoch + pool->capacityEpoch();
    }

    BackgroundWorkerStorage *workerStorage() noexcept override {
      return pool->workerStorage();
    }
  };

  for (bool deferred : {false, true}) {
    GateExecutor executor;
    Fixture f(80, {}, {}, {}, false, &executor);
    f.pump();
    executor.full = deferred;
    executor.rejected = BackgroundError::NO_MEMORY;
    auto admitted = f.choice.select(65);
    CHECK(bool(admitted) == deferred);
    if (!deferred) {
      CHECK(admitted.error().background == BackgroundError::NO_MEMORY);
    }
    executor.full = false;
    ++executor.epoch;
    f.pump();
    const auto submissions = executor.submissions;
    executor.rejected.reset();
    ++executor.epoch;
    for (int turn = 0; turn < 10; ++turn) {
      f.pump();
    }
    CHECK(executor.submissions == submissions);
    CHECK_FALSE(take(f.choice.selection()));
    CHECK(f.probe->reads == 0);
    auto diagnostics = take(f.scene->diagnostics());
    REQUIRE(diagnostics.errors.size() == 1);
    CHECK(diagnostics.errors.front().error.background == BackgroundError::NO_MEMORY);
    take(f.choice.select(65));
    REQUIRE(f.until([&] {
      return bool(take(f.choice.selection()));
    }));
    CHECK(take(f.choice.selection()).index() == 65);
    executor.pool->requestStop();
    executor.pool->join();
  }
}

TEST_CASE("lazy open coalesces and pending dismissal leaves the main control intact") {
  Fixture f;
  f.pump();
  take(f.choice.open());
  take(f.choice.open());
  CHECK_FALSE(take(f.choice.opened()));
  take(f.choice.dismiss());
  for (int turn = 0; turn < 3; ++turn) {
    f.pump();
  }
  CHECK(f.popup() == Node<Dialog>{});
  CHECK(f.probe->reads == 0);
  for (int cycle = 0; cycle < 40; ++cycle) {
    take(f.choice.open());
    REQUIRE(f.until([&] {
      return take(f.choice.opened());
    }));
    take(f.choice.dismiss());
    f.pump();
    CHECK(f.popup() == Node<Dialog>{});
  }
  CHECK_FALSE(take(f.choice.selection()));
}

TEST_CASE("more than32 closed model-backed choices do not reserve popup layers or result models") {
  auto pool = take(BackgroundPool::create());
  auto app = take(Application::create(ApplicationOptions{.backgroundExecutor = pool.get()}));
  auto scene = take(ManagedScene::create(*app));
  auto probe = std::make_shared<Probe>();
  auto model = take(scene->model<std::string>(std::make_shared<const Rows>(probe)));
  std::vector<ui::Description<ComboBox>> choices;
  for (int index = 0; index < 80; ++index) {
    choices.push_back(ui::comboBox(model, [](const std::string &text) -> std::string_view {
      return text;
    }));
  }
  take(ui::mount(*scene, ui::column(std::move(choices))));
  for (int turn = 0; turn < 3; ++turn) {
    choice_test::pump(*app, *scene, *pool, {40, 4});
  }
  CHECK(probe->reads == 0);
  CHECK(take(app->tree().consumeFrameRequest()) == Invalidation::NONE);
  take(scene->close());
  pool->requestStop();
  pool->join();
}

TEST_CASE("choice callbacks may remove their owner and retained selection bytes remain readable") {
  Fixture f;
  f.pump();
  ChoiceSelection saved;
  take(ui::detail::choiceSelectHandler(
    ui::detail::NamedAccess::target(f.choice),
    [&](ChoiceSelection value) {
      saved = value;
      take(f.scene->remove(f.body));
    },
    std::source_location::current()));
  take(f.choice.select(65));
  REQUIRE(f.until([&] {
    return bool(saved);
  }));
  CHECK(saved.text() == "Duplicate");
  CHECK_FALSE(f.choice.selection());
}

class LastRow final : public ModelSnapshot<std::string> {
public:
  static constexpr std::uint64_t ROWS = 200001;

  LastRow() : ModelSnapshot(1, ROWS) {
  }

  ModelRows<std::string> read(RowRange range) const override {
    reads += range.count;
    if (range == RowRange{ROWS - 1, 1}) {
      return {RangeState::READY, row, {}};
    }
    return {};
  }

  mutable std::atomic<std::size_t> reads{};

private:
  std::array<ModelRow<std::string>, 1> row{{{{ROWS, 1}, "Last row"}}};
};

TEST_CASE("one-row ordinal selection beyond the indexed search source cap never builds an index") {
  auto source = std::make_shared<const LastRow>();
  Fixture f(0, {}, {}, source);
  f.pump();
  take(f.choice.select(LastRow::ROWS - 1));
  REQUIRE(f.until([&] {
    return bool(take(f.choice.selection()));
  }));
  CHECK(take(f.choice.selection()).index() == LastRow::ROWS - 1);
  CHECK(source->reads == 1);
  CHECK(f.probe->projections == 1);
}

TEST_CASE("Escape dismisses a popup without reverting immediate edits and ordinary Undo still works") {
  Fixture f;
  f.pump();
  take(f.choice.editor().focus());
  f.input(TextInputEvent{U'A'});
  REQUIRE(f.until([&] {
    return take(f.choice.opened());
  }));
  f.input(KeyEvent{InputKey::ESCAPE});
  CHECK_FALSE(take(f.choice.opened()));
  CHECK(take(f.choice.text().get()) == "A");
  CHECK_FALSE(take(f.choice.selection()));
  take(f.choice.editor().undo().result());
  CHECK(take(f.choice.text().get()).empty());
}

TEST_CASE("DropDown rejects user text and keyboard highlight does not accept before Enter") {
  Fixture f;
  f.pump();
  take(f.readOnly.editor().focus());
  f.input(TextInputEvent{U'X'});
  CHECK(take(f.readOnly.editor().text().get()).empty());
  take(f.choice.open());
  REQUIRE(f.until([&] {
    return take(f.choice.opened());
  }));
  auto list = f.browse();
  REQUIRE(f.until([&] {
    bool ready{};
    take(f.app->tree().read(list, [&](const auto &view) {
      ready = view.rangeState() == RangeState::READY;
    }));
    return ready;
  }));
  take(f.app->router().setFocus(list));
  f.input(KeyEvent{InputKey::DOWN});
  CHECK_FALSE(take(f.choice.selection()));
  f.input(KeyEvent{InputKey::ENTER});
  REQUIRE(f.until([&] {
    return bool(take(f.choice.selection()));
  }));
  CHECK(f.notices.size() == 1);
}

TEST_CASE("a lazy popup failure removes only its attempted models and preserves main references and draft") {
  Fixture f(80, {}, ManagedOptions{.maxModels = 3});
  f.pump();
  take(f.choice.text().set("Preserved").result());
  for (int attempt = 0; attempt < 3; ++attempt) {
    take(f.choice.open());
    f.pump();
    CHECK_FALSE(take(f.choice.opened()));
    CHECK(f.popup() == Node<Dialog>{});
    CHECK(take(f.choice.text().get()) == "Preserved");
  }
  auto diagnostics = take(f.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 3);
  for (const auto &diagnostic : diagnostics.errors) {
    CHECK(std::get<TreeError>(diagnostic.error.cause) == TreeError::NODE_LIMIT);
    CHECK(std::string_view(diagnostic.error.operation) == "ComboBox.mountPopup");
  }
}

TEST_CASE("hiding between popup mount and final geometry cancels its pending activation") {
  Fixture f;
  f.pump();
  take(f.choice.open());
  take(f.scene->afterDispatch(*f.app));
  REQUIRE(f.popup() != Node<Dialog>{});
  CHECK_FALSE(take(f.choice.opened()));
  take(f.app->tree().setVisible(f.choice.node(), false));
  f.pump();
  CHECK_FALSE(take(f.choice.opened()));
  CHECK(f.popup() == Node<Dialog>{});
  CHECK(f.probe->reads == 0);
}

TEST_CASE("typed loaded aliases mount one automatic host and invoke their factory exactly once") {
  int factories{};
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.control<ComboBox>("CityChoice", [&](ui::SceneResourceArguments args) {
    ++factories;
    return ui::comboBox({"One", "Two"}).settings(args.settings);
  });
  auto scene = ui::scene(ui::vbox(
    ui::label("Before"),
    ui::resource("scene { VBox { CityChoice#city { text=\"Draft\" } DropDown#delivery { placeholder=\"Choose\" } } }",
                 options)));
  auto choice = scene.get<ComboBox>("city");
  auto delivery = scene.get<DropDown>("delivery");
  CHECK(factories == 0);
  take(scene.prepareChecked());
  CHECK(factories == 1);
  CHECK(take(choice.text().get()) == "Draft");
  CHECK_FALSE(take(delivery.selection()));
}

TEST_CASE("framed choice roots bind the actual outer body while preserving the inner reference") {
  ui::Ref<Column> body;
  ui::Ref<ComboBox> choice;
  auto scene = ui::scene(ui::vbox(body, ui::comboBox(choice, {"One", "Two"})).width(78).framed("rounded", "Choices"));
  take(scene.prepareChecked());
  CHECK(body.node() != Node<Column>{});
  CHECK(choice.node() != Node<ComboBox>{});
  ui::Ref<ComboBox> explicitChoice;
  auto explicitHost = ui::scene(ui::overlayHost(ui::comboBox(explicitChoice, {"One"}).framed("rounded", "Explicit")));
  take(explicitHost.prepareChecked());
  CHECK(explicitChoice.node() != Node<ComboBox>{});
}

TEST_CASE("an owning formatted projection survives worker return and source retirement") {
  Fixture f(80, {}, {}, {}, true);
  f.pump();
  take(f.choice.select(65));
  REQUIRE(f.until([&] {
    return bool(take(f.choice.selection()));
  }));
  auto selected = take(f.choice.selection());
  CHECK(selected.text() == "Place: Duplicate");
  CHECK(f.probe->projections == 1);
  CHECK(f.probe->workerOnly);
  take(f.scene->close());
  CHECK(selected.text() == "Place: Duplicate");
}

TEST_CASE("indexed completion copies owning formatted labels while its build scratch remains bounded") {
  Fixture f(80, {}, {}, {}, true);
  f.pump();
  take(f.choice.text().set("Place: Dup").result());
  take(f.choice.open());
  REQUIRE(f.until([&] {
    return take(f.choice.opened());
  }));
  auto list = f.popupList(2);
  REQUIRE(f.until([&] {
    bool ready{};
    take(f.app->tree().read(list, [&](const auto &view) {
      ready = view.rangeState() == RangeState::READY && view.dataStatus().rowCount == 2;
    }));
    return ready;
  }));
  take(setVirtualListSelection(f.app->tree(), list, RowId{66, 1}));
  take(f.app->router().invoke(list, {portals::Action::ACTIVATE}));
  REQUIRE(f.until([&] {
    return bool(take(f.choice.selection()));
  }));
  CHECK(take(f.choice.selection()).text() == "Place: Duplicate");
  CHECK(take(f.choice.selection()).index() == 65);
  CHECK(f.probe->workerOnly);
  CHECK(f.errors.empty());
}

TEST_CASE("owning projection label limits are rejected before retaining output") {
  ui::ComboBoxConfig config;
  config.search.maxRowBytes = 8;
  Fixture f(80, config, {}, {}, true);
  f.pump();
  take(f.choice.select(0));
  REQUIRE(f.until([&] {
    return !f.errors.empty();
  }));
  CHECK(f.errors.front() == SearchError::LIMIT_EXCEEDED);
  CHECK_FALSE(take(f.choice.selection()));
  auto diagnostics = take(f.scene->diagnostics());
  REQUIRE_FALSE(diagnostics.errors.empty());
  CHECK(diagnostics.errors.front().error.search == SearchError::LIMIT_EXCEEDED);
}

TEST_CASE("projection and failure callbacks retain exact provider errors in the collector") {
  ui::ComboBoxConfig config;
  config.failed = [](SearchError) {
    throw std::runtime_error("failure callback");
  };
  Fixture f(80, config);
  f.pump();
  f.probe->throwProjection = true;
  take(f.choice.select(0));
  REQUIRE(f.until([&] {
    return !f.errors.empty();
  }));
  CHECK(f.errors.front() == SearchError::CALLBACK_FAILED);
  auto diagnostics = take(f.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 2);
  CHECK(diagnostics.errors[0].error.search == SearchError::CALLBACK_FAILED);
  CHECK(diagnostics.errors[1].error.search == SearchError::CALLBACK_FAILED);
  CHECK(std::get<TreeError>(diagnostics.errors[1].error.cause) == TreeError::CALLBACK_FAILED);
}

TEST_CASE("Down and Enter retain their intent through pending popup geometry and worker delivery") {
  Fixture f;
  f.pump();
  take(f.choice.editor().focus());
  const auto inputWithoutFrame = [&](InputEvent event) {
    if (!take(f.scene->input(*f.app, event))) {
      take(f.app->router().dispatch(event));
    }
  };
  inputWithoutFrame(TextInputEvent{U'D'});
  inputWithoutFrame(KeyEvent{InputKey::DOWN});
  inputWithoutFrame(KeyEvent{InputKey::ENTER});
  CHECK_FALSE(take(f.choice.selection()));
  REQUIRE(f.until([&] {
    return bool(take(f.choice.selection()));
  }));
  CHECK(take(f.choice.selection()).text() == "Duplicate");
  CHECK(f.notices.size() == 1);
  CHECK_FALSE(take(f.choice.opened()));
}

TEST_CASE("pending popup navigation retains Home End Up and page commands before the first frame") {
  struct Case {
    std::vector<InputKey> keys;
    std::uint64_t selected;
  };

  const std::array cases{Case{{InputKey::END}, 79}, Case{{InputKey::END, InputKey::HOME}, 0},
                         Case{{InputKey::END, InputKey::UP}, 78}, Case{{InputKey::PAGE_DOWN}, 8},
                         Case{{InputKey::PAGE_DOWN, InputKey::PAGE_UP}, 0}};
  for (const auto &sample : cases) {
    Fixture f;
    f.pump();
    take(f.choice.editor().focus());
    const auto input = [&](InputKey key) {
      const InputEvent event = KeyEvent{key};
      if (!take(f.scene->input(*f.app, event))) {
        take(f.app->router().dispatch(event));
      }
    };
    input(InputKey::DOWN);
    for (auto key : sample.keys) {
      input(key);
    }
    input(InputKey::ENTER);
    REQUIRE(f.until([&] {
      return bool(take(f.choice.selection()));
    }));
    CHECK(take(f.choice.selection()).index() == sample.selected);
    CHECK(f.probe->rows <= 64);
  }
}

TEST_CASE("editing after pending navigation cancels the old acceptance intent") {
  Fixture f;
  f.pump();
  take(f.choice.editor().focus());
  const auto inputWithoutFrame = [&](InputEvent event) {
    if (!take(f.scene->input(*f.app, event))) {
      take(f.app->router().dispatch(event));
    }
  };
  inputWithoutFrame(TextInputEvent{U'D'});
  inputWithoutFrame(KeyEvent{InputKey::DOWN});
  inputWithoutFrame(KeyEvent{InputKey::ENTER});
  inputWithoutFrame(TextInputEvent{U'Z'});
  REQUIRE(f.until([&] {
    return take(f.choice.opened());
  }));
  for (int turn = 0; turn < 20; ++turn) {
    f.pump();
    std::this_thread::sleep_for(1ms);
  }
  CHECK(take(f.choice.text().get()) == "DZ");
  CHECK_FALSE(take(f.choice.selection()));
  CHECK(f.notices.empty());
}

TEST_CASE("mouse acceptance uses the clicked row rather than the previous highlight") {
  for (bool search : {false, true}) {
    Fixture f;
    f.pump();
    if (search) {
      take(f.choice.text().set("Duplicate").result());
    }
    take(f.choice.open());
    REQUIRE(f.until([&] {
      return take(f.choice.opened());
    }));
    const auto list = f.popupList(search ? 2 : 1);
    REQUIRE(f.until([&] {
      bool ready{};
      take(f.app->tree().read(list, [&](const auto &view) {
        ready = view.rangeState() == RangeState::READY && (!search || view.dataStatus().rowCount == 2);
      }));
      return ready;
    }));
    take(setVirtualListSelection(f.app->tree(), list, RowId{1, 1}));
    const auto geometry = take(f.app->tree().inspect(list));
    const auto origin = geometry.geometry.surface.origin();
    f.input(MouseInputEvent{origin.x, origin.y + 1, MouseButton::LEFT, MouseAction::PRESS});
    REQUIRE(f.until([&] {
      return bool(take(f.choice.selection()));
    }));
    CHECK(take(f.choice.selection()).index() == (search ? 65 : 1));
    CHECK(take(f.choice.selection()).text() == "Duplicate");
    CHECK_FALSE(take(f.choice.opened()));
  }
}

TEST_CASE("protected editor display modes reject before choice Ref publication") {
  for (auto mode : {LineEditDisplayMode::PASSWORD, LineEditDisplayMode::NO_ECHO}) {
    auto pool = take(BackgroundPool::create());
    auto app = take(Application::create(ApplicationOptions{.backgroundExecutor = pool.get()}));
    auto scene = take(ManagedScene::create(*app));
    ui::Ref<ComboBox> ref;
    ui::ComboBoxConfig config;
    config.editor.options.displayMode = mode;
    auto mounted = ui::mount(*scene, ui::comboBox(ref, {"One"}, config));
    REQUIRE_FALSE(mounted);
    CHECK(std::get<TreeError>(mounted.error().cause) == TreeError::INVALID_ARGUMENT);
    CHECK(ref.node() == Node<ComboBox>{});
    pool->requestStop();
    pool->join();
  }
}

TEST_CASE("raw tree removal retires the live lazy popup and its private result owners") {
  struct ReplacementActivity final : ui::detail::SceneActivity {
    bool active() const noexcept override {
      return true;
    }

    void service() noexcept override {
    }

    void cancel() noexcept override {
    }
  };

  Fixture f(80, {}, ManagedOptions{.maxModels = 4});
  f.pump();
  take(f.choice.open());
  REQUIRE(f.until([&] {
    return take(f.choice.opened());
  }));
  const auto popup = f.popup();
  take(f.app->tree().remove(f.choice.node()));
  auto replacement = std::make_shared<ReplacementActivity>();
  take(ui::detail::SceneAccess::admitActivity(*f.scene, replacement));
  CHECK_FALSE(f.app->tree().inspect(popup));
  f.pump();
  CHECK_FALSE(f.app->tree().inspect(popup));
  CHECK_FALSE(f.choice.selection());
  for (int cycle = 0; cycle < 40; ++cycle) {
    take(f.readOnly.open());
    REQUIRE(f.until([&] {
      return take(f.readOnly.opened());
    }));
    take(f.readOnly.dismiss());
    f.pump();
  }
  CHECK(take(f.scene->diagnostics()).errors.empty());
}

TEST_CASE("input callbacks may compact and append the existing activity registry") {
  struct InputActivity final : ui::detail::SceneActivity {
    bool live{true};
    unsigned calls{};
    std::function<void()> callback;

    bool active() const noexcept override {
      return live;
    }

    void service() noexcept override {
    }

    void cancel() noexcept override {
      live = false;
    }

    std::expected<bool, TreeError> input(const InputEvent &) override {
      ++calls;
      if (callback) {
        callback();
      }
      return false;
    }
  };

  Fixture f;
  auto first = std::make_shared<InputActivity>();
  auto expired = std::make_shared<InputActivity>();
  auto replacement = std::make_shared<InputActivity>();
  take(ui::detail::SceneAccess::admitActivity(*f.scene, first));
  take(ui::detail::SceneAccess::admitActivity(*f.scene, expired));
  first->callback = [&] {
    first->live = false;
    expired->live = false;
    take(ui::detail::SceneAccess::admitActivity(*f.scene, replacement));
  };
  CHECK_FALSE(take(f.scene->input(*f.app, KeyEvent{InputKey::F10})));
  CHECK(first->calls == 1);
  CHECK(expired->calls == 0);
  CHECK(replacement->calls == 0);
  CHECK_FALSE(take(f.scene->input(*f.app, KeyEvent{InputKey::F10})));
  CHECK(replacement->calls == 1);
}

TEST_CASE("a later public onSelect callback retires an earlier choice popup in the same service pass") {
  auto pool = take(BackgroundPool::create());
  ui::Ref<ComboBox> earlier;
  ui::Ref<DropDown> later;
  auto scene = ui::scene(ui::vbox(ui::comboBox(earlier, {"One", "Two"}), ui::dropDown(later, {"Courier", "Pickup"})));
  scene.applicationOptions([&] {
    return ApplicationOptions{.backgroundExecutor = pool.get()};
  });
  scene | ui::onSelect(later, [&scene, earlier](ChoiceSelection selected) {
    if (selected) {
      auto &managed = take(scene.managed()).get();
      take(ui::detail::SceneAccess::application(managed).tree().remove(earlier.node()));
    }
  });
  take(scene.prepareChecked());
  auto &managed = take(scene.managed()).get();
  auto &app = ui::detail::SceneAccess::application(managed);
  const auto pump = [&] {
    choice_test::pump(app, managed, *pool, {80, 20});
  };
  const auto until = [&](auto predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
      pump();
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  };
  pump();
  take(earlier.open());
  REQUIRE(until([&] {
    return take(earlier.opened());
  }));
  Node<Dialog> popup;
  take(app.tree().read(earlier.node(), [&](const ComboBox &choice) {
    popup = choice.popup();
  }));
  take(later.select(1));
  REQUIRE(until([&] {
    return bool(take(later.selection()));
  }));
  CHECK_FALSE(app.tree().inspect(earlier.node()));
  CHECK_FALSE(app.tree().inspect(popup));
  CHECK(take(managed.diagnostics()).errors.empty());
  take(managed.close());
  pool->requestStop();
  pool->join();
}

TEST_CASE("failed activity retirement retains its weak observation until a later ordinary cycle") {
  struct Retiring final : ui::detail::SceneActivity {
    unsigned attempts{};

    bool active() const noexcept override {
      return false;
    }

    void service() noexcept override {
    }

    void cancel() noexcept override {
    }

    bool tryRetire() noexcept override {
      return ++attempts > 1;
    }
  };

  Fixture f;
  auto retiring = std::make_shared<Retiring>();
  take(ui::detail::SceneAccess::admitActivity(*f.scene, retiring));
  take(f.scene->afterDispatch(*f.app));
  CHECK(retiring->attempts == 1);
  take(f.scene->afterDispatch(*f.app));
  CHECK(retiring->attempts == 2);
  take(f.scene->afterDispatch(*f.app));
  CHECK(retiring->attempts == 2);
}
} // namespace
