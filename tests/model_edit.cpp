// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>
#include <portals/ui_model_edit.hpp>

#include <array>
#include <atomic>
#include <cstdlib>

namespace {
std::atomic<std::ptrdiff_t> editAllocationsBeforeFailure{-1};
}

// Fault injection is armed only around one checked field operation, never the runtime as a whole.
void *operator new(std::size_t size) {
  if (editAllocationsBeforeFailure.load() == 0) {
    throw std::bad_alloc();
  }
  if (editAllocationsBeforeFailure.load() > 0) {
    --editAllocationsBeforeFailure;
  }
  if (auto *value = std::malloc(size ? size : 1)) {
    return value;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void *value) noexcept {
  std::free(value);
}

void operator delete[](void *value) noexcept {
  std::free(value);
}

void operator delete(void *value, std::size_t) noexcept {
  std::free(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  std::free(value);
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
  ::operator delete[](value);
}

namespace {
using namespace portals;

template <class T, class E>
T take(std::expected<T, E> result, std::source_location where = std::source_location::current()) {
  INFO(where.file_name(), ":", where.line());
  if constexpr (std::same_as<E, TreeError>) {
    if (!result) {
      INFO("TreeError=", static_cast<int>(result.error()));
      REQUIRE(result);
    }
  }
  REQUIRE(result);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct Contact {
  std::string name;
  std::string country;
  int age{};
  bool operator==(const Contact &) const = default;
};

struct Sink final : PaintContext {
  std::expected<void, TreeError> submit(std::span<const RenderCommand>) override {
    return {};
  }
};

struct CellPaint final : PaintContext {
  NodeGeometry geometry;
  std::vector<RenderCommand> &commands;

  CellPaint(NodeGeometry value, std::vector<RenderCommand> &target) : geometry(value), commands(target) {
  }

  std::expected<void, TreeError> submit(std::span<const RenderCommand> input) override {
    for (const auto &command : input) {
      std::visit(
        [&](const auto &value) {
          const auto clip = take(value.clip.translated(geometry.surface.origin())).intersection(geometry.clip);
          if constexpr (std::same_as<std::decay_t<decltype(value)>, TextCommand>) {
            const auto origin = take(translated(value.origin, geometry.surface.origin()));
            commands.emplace_back(
              TextCommand{origin, value.text, clip, value.style, value.z, value.firstGrapheme, value.graphemeCount});
          } else {
            const auto area = take(value.area.translated(geometry.surface.origin()));
            commands.emplace_back(FillCommand{area, clip, value.style, value.z});
          }
        },
        command);
    }
    return {};
  }
};

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  ui::Model<Contact> model = ui::model(std::array{Contact{"A", "AA", 1}, Contact{"B", "BB", 2}});
  ui::Navigation<Contact> navigation = ui::navigation(model);
  ui::EditSession<Contact> edit;
  ui::EditField<Contact, std::string> name;
  ui::EditField<Contact, std::string> country;
  ui::Ref<LineEdit> nameEditor;
  ui::Ref<LineEdit> countryEditor;
  ui::Ref<VirtualList<Contact>> list;
  Node<Widget> content;

  explicit Fixture(EditMode mode = EditMode::EXPLICIT, ui::LineEditConfig config = {}, EditOptions options = {},
                   BackgroundExecutor *executor = nullptr, SchedulerClock clock = {})
      : app(take(Application::create({.backgroundExecutor = executor}, clock))) {
    options.mode = mode;
    edit = ui::edit(navigation, options);
    name = edit.field("contact.name", &Contact::name);
    country = edit.field("contact.country", &Contact::country);
    content = take(ui::mount(*scene, ui::vbox(ui::virtualList(navigation, &Contact::name).height(3).reference(list),
                                              ui::lineEdit(nameEditor, name, std::move(config)),
                                              ui::lineEdit(countryEditor, country))));
    take(take(navigation.managed()).navigate({1, 1}));
    paint();
    take(app->router().setFocus(nameEditor.node()));
  }

  template <class T> std::expected<Node<T>, ManagedError> mount(ui::Description<T> description) {
    return ui::detail::mountAt(*scene, content, std::move(description), ui::ControlContext{});
  }

  void paint() {
    take(scene->prepare(*app, {60, 12}));
    const auto bounds = take(CellRect::from({}, {60, 12}));
    std::optional<LayoutSnapshot> layout;
    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
      layout = take(LayoutSnapshot::capture(app->tree(), app->root()));
    }));
    take(scene->prepareViews(*app, *layout));
    Sink sink;
    take(app->tree().frame([&] {
      take(app->tree().paint(nameEditor.node(), sink));
      take(app->tree().paint(countryEditor.node(), sink));
    }));
  }

  std::string renderCells() {
    paint();
    std::optional<CellBuffer> cells;
    take(app->tree().frame([&] {
      std::vector<RenderCommand> commands;
      auto visit = [&](auto &&self, Node<Widget> node) -> void {
        const auto snapshot = take(app->tree().inspect(node));
        if (!snapshot.visible || !snapshot.geometry.valid) {
          return;
        }
        CellPaint sink(snapshot.geometry, commands);
        take(app->tree().paint(node, sink));
        for (const auto child : snapshot.children) {
          self(self, child);
        }
      };
      visit(visit, app->root());
      // Consume borrowed text commands before the enclosing frame releases them.
      cells.emplace(take(renderFullFrame({60, 12}, commands)));
    }));
    std::string result;
    for (std::int32_t y = 0; y < cells->size().height; ++y) {
      for (std::int32_t x = 0; x < cells->size().width; ++x) {
        const auto cell = take(cells->at({x, y}));
        if (cell.cell.kind != CellKind::CONTINUATION) {
          result += cell.glyph;
        }
      }
      result += '\n';
    }
    return result;
  }

  Contact value(RowId row) {
    const auto data = ui::detail::EditAccess::sequence(model)->current;
    const auto found = data->locateRow(row);
    REQUIRE(found.state == RowLookupState::FOUND);
    return data->rows()[found.index].value;
  }

  void go(RowId row) {
    take(take(navigation.managed()).navigate(row));
  }

  std::string text() {
    return take(nameEditor.text().get());
  }
};

TEST_CASE("explicit fields preserve A B A draft and commit independent own fields") {
  Fixture fixture;
  take((fixture.nameEditor.text() = "A draft").result());
  take((fixture.countryEditor.text() = "AA draft").result());
  const auto original = take(fixture.name.snapshot()).target;
  fixture.go({2, 1});
  fixture.paint();
  CHECK(fixture.text() == "B");
  take((fixture.nameEditor.text() = "B draft").result());
  fixture.go({1, 1});
  fixture.paint();
  CHECK(fixture.text() == "A draft");
  CHECK(fixture.value({1, 1}).name == "A");
  take(fixture.edit.commit().result());
  CHECK(fixture.value({1, 1}).name == "A draft");
  CHECK(fixture.value({1, 1}).country == "AA draft");
  CHECK(fixture.value({2, 1}).name == "B");
  fixture.go({2, 1});
  fixture.paint();
  CHECK(fixture.text() == "B draft");
  take(fixture.edit.cancel().result());
  CHECK(fixture.text() == "B");
  CHECK(take(fixture.name.snapshot(original)).accepted);
}

TEST_CASE("displayed target survives current change and two events without paint") {
  Fixture fixture(EditMode::IMMEDIATE);
  fixture.go({2, 1});
  take(fixture.app->router().dispatch(TextInputEvent{U'x'}));
  take(fixture.app->router().dispatch(TextInputEvent{U'y'}));
  CHECK(fixture.value({1, 1}).name == "Axy");
  CHECK(fixture.value({2, 1}).name == "B");
  fixture.paint();
  CHECK(fixture.text() == "B");
}

TEST_CASE("completion validation vetoes user current change while target exists") {
  ui::LineEditConfig config;
  config.validate = [](std::string_view text) {
    return !text.empty();
  };
  Fixture fixture(EditMode::ON_COMPLETION, std::move(config));
  take((fixture.nameEditor.text() = "").result());
  auto result = take(fixture.navigation.managed()).navigate({2, 1});
  CHECK_FALSE(result);
  CHECK(take(fixture.navigation.currentRow()) == RowId{1, 1});
  CHECK(fixture.text().empty());
  CHECK(fixture.value({1, 1}).name == "A");
}

TEST_CASE("immediate invalid draft is retained on its original record") {
  ui::LineEditConfig config;
  config.validate = [](std::string_view text) {
    return text != "invalid";
  };
  Fixture fixture(EditMode::IMMEDIATE, std::move(config));
  const auto target = take(fixture.name.snapshot()).target;
  take((fixture.nameEditor.text() = "invalid").result());
  fixture.go({2, 1});
  fixture.paint();
  CHECK(*take(fixture.name.snapshot(target)).draft == "invalid");
  CHECK(fixture.value({1, 1}).name == "A");
  fixture.go({1, 1});
  fixture.paint();
  CHECK(fixture.text() == "invalid");
}

TEST_CASE("external same-record update conflicts and unrelated row does not") {
  Fixture fixture;
  take((fixture.nameEditor.text() = "draft").result());
  take(fixture.model.update({2, 1}, Contact{"other", "BB", 2}));
  take(fixture.edit.commit().result());
  CHECK(fixture.value({1, 1}).name == "draft");
  take((fixture.nameEditor.text() = "stale").result());
  take(fixture.model.update({1, 1}, Contact{"external", "AA", 1}));
  CHECK_FALSE(fixture.edit.commit());
  CHECK(fixture.value({1, 1}).name == "external");
  CHECK(fixture.text() == "stale");
}

TEST_CASE("typed non-string field uses record versions and explicit target inspection") {
  Fixture fixture;
  auto age = fixture.edit.field("contact.age", &Contact::age);
  const auto target = take(age.snapshot()).target;
  CHECK(take(age.set(42)) == EditStatus::DIRTY);
  fixture.go({2, 1});
  CHECK(*take(age.snapshot(target)).draft == 42);
  fixture.go({1, 1});
  fixture.paint();
  take(fixture.edit.commit().result());
  CHECK(fixture.value({1, 1}).age == 42);
  age.validate([](int value) {
    return value >= 0;
  });
  take(age.set(-5));
  CHECK_FALSE(fixture.edit.commit());
  CHECK(fixture.value({1, 1}).age == 42);
  CHECK(*take(age.snapshot()).draft == -5);
  take(fixture.edit.cancel().result());
}

TEST_CASE("forced deletion keeps an inspectable orphan and cannot write nearest") {
  Fixture fixture;
  const auto target = take(fixture.name.snapshot()).target;
  take((fixture.nameEditor.text() = "orphan").result());
  take(fixture.model.erase(0));
  fixture.paint();
  CHECK(*take(fixture.name.snapshot(target)).draft == "orphan");
  CHECK(fixture.value({2, 1}).name == "B");
  take(fixture.name.discard(target));
  CHECK_FALSE(fixture.name.snapshot(target));
}

TEST_CASE("budget refusal preserves previously accepted draft bytes") {
  Fixture fixture(EditMode::EXPLICIT, {}, {.maxRetainedFields = 4, .maxRetainedBytes = 5000});
  take((fixture.nameEditor.text() = "keep").result());
  const auto tooLarge = fixture.nameEditor.text() = std::string(1000, 'x');
  CHECK_FALSE(tooLarge.result());
  CHECK(fixture.text() == "keep");
  CHECK(fixture.value({1, 1}).name == "A");
}
} // namespace

namespace {
class DeferredIntEndpoint final : public EditEndpoint<int> {
public:
  explicit DeferredIntEndpoint(bool conditional = true) : conditional_(conditional) {
  }

  EditCapabilities capabilities() const noexcept override {
    return {true, conditional_, conditional_, false};
  }

  std::expected<EditRecord<int>, TreeError> read(ModelResource, RowId) override {
    if (backendGeneration != boundGeneration) {
      return std::unexpected(TreeError::CLOSED);
    }
    return EditRecord<int>{std::make_shared<const int>(value), version};
  }

  std::expected<EditReceipt, TreeError> submit(EditRequest<int> request) override {
    if (submitFailure) {
      return std::unexpected(*submitFailure);
    }
    if (backendGeneration != boundGeneration) {
      return EditReceipt{request.target, EditStatus::CLOSED};
    }
    requests.push_back(std::move(request));
    return EditReceipt{requests.back().target, EditStatus::PENDING};
  }

  std::expected<std::size_t, TreeError> ownedBytes(const int &) const override {
    return chargeOverride.value_or(sizeof(int));
  }

  EditReceipt accept(std::size_t index) {
    const auto &request = requests.at(index);
    if (backendGeneration != boundGeneration) {
      return {request.target, EditStatus::CLOSED};
    }
    if ((conditional_ && request.target.baseVersion != version) || request.target.sequence <= acceptedSequence) {
      return {request.target, EditStatus::CONFLICT};
    }
    value = *request.value;
    ++version;
    acceptedSequence = request.target.sequence;
    return {request.target, EditStatus::ACCEPTED, version};
  }

  std::vector<EditRequest<int>> requests;
  std::optional<std::size_t> chargeOverride;
  std::optional<TreeError> submitFailure;
  std::uint64_t backendGeneration{1};
  const std::uint64_t boundGeneration{1};
  int value{7};
  std::uint64_t version{1};
  std::uint64_t acceptedSequence{};

private:
  bool conditional_;
};

TEST_CASE("conditional endpoint reverse acceptance and reverse receipts never roll backend back") {
  Fixture fixture(EditMode::IMMEDIATE);
  auto endpoint = std::make_shared<DeferredIntEndpoint>();
  auto field = fixture.edit.field("remote.age", &Contact::age, endpoint);
  CHECK(take(field.set(10)) == EditStatus::PENDING);
  CHECK(take(field.set(20)) == EditStatus::PENDING);
  REQUIRE(endpoint->requests.size() == 2);
  auto scope = take(fixture.app->scheduler().makeScope());
  std::vector<bool> applied;
  auto delivery =
    take(fixture.app->scheduler().connect<EditReceipt>(scope, [edit = fixture.edit, &applied](auto receipt) {
      applied.push_back(take(edit.applyReply(*receipt)));
    }));
  bool queued{};
  EditReceipt second;
  EditReceipt first;
  std::thread worker([&] {
    second = endpoint->accept(1);
    first = endpoint->accept(0);
    const auto queuedSecond = delivery.publisher.publish(std::make_shared<const EditReceipt>(second));
    const auto queuedFirst = delivery.publisher.publish(std::make_shared<const EditReceipt>(first));
    queued = queuedSecond.has_value() && queuedFirst.has_value();
  });
  worker.join();
  REQUIRE(queued);
  CHECK(applied.empty());
  take(fixture.app->scheduler().dispatch());
  CHECK(second.status == EditStatus::ACCEPTED);
  CHECK(first.status == EditStatus::CONFLICT);
  REQUIRE(applied.size() == 2);
  CHECK(applied[0]);
  CHECK_FALSE(applied[1]);
  CHECK(endpoint->value == 20);
  CHECK(*take(field.snapshot()).accepted == 20);
  CHECK_FALSE(take(field.snapshot()).draft);
}

TEST_CASE("unconditional endpoint serializes backend work and stale ack keeps newer draft") {
  Fixture fixture(EditMode::IMMEDIATE);
  auto endpoint = std::make_shared<DeferredIntEndpoint>(false);
  auto field = fixture.edit.field("remote.serial-age", &Contact::age, endpoint);
  CHECK(take(field.set(10)) == EditStatus::PENDING);
  CHECK(take(field.set(20)) == EditStatus::PENDING);
  REQUIRE(endpoint->requests.size() == 1);
  auto first = endpoint->accept(0);
  CHECK(take(fixture.edit.applyReply(first)));
  CHECK(*take(field.snapshot()).draft == 20);
  REQUIRE(endpoint->requests.size() == 2);
  take(fixture.edit.commit().result());
  REQUIRE(endpoint->requests.size() == 2);
  CHECK(endpoint->requests.back().target.baseVersion == 2);
  auto second = endpoint->accept(1);
  CHECK(take(fixture.edit.applyReply(second)));
  CHECK_FALSE(take(fixture.edit.applyReply(first)));
  CHECK(endpoint->value == 20);
}

TEST_CASE("cancel preserves admitted serial work and discard cannot reopen its slot") {
  Fixture fixture(EditMode::IMMEDIATE);
  auto endpoint = std::make_shared<DeferredIntEndpoint>(false);
  auto field = fixture.edit.field("remote.cancel-age", &Contact::age, endpoint);
  take(field.set(10));
  const auto target = take(field.snapshot()).target;
  take(fixture.edit.cancel().result());
  CHECK_FALSE(field.discard(target));
  take(field.set(20));
  REQUIRE(endpoint->requests.size() == 1);
  CHECK(take(fixture.edit.applyReply(endpoint->accept(0))));
  REQUIRE(endpoint->requests.size() == 2);
  CHECK(take(fixture.edit.applyReply(endpoint->accept(1))));
  CHECK(endpoint->value == 20);
}

TEST_CASE("delayed endpoint checks its backend incarnation independently of UI source identity") {
  Fixture fixture(EditMode::IMMEDIATE);
  auto endpoint = std::make_shared<DeferredIntEndpoint>();
  auto field = fixture.edit.field("remote.incarnation-age", &Contact::age, endpoint);
  take(field.set(10));
  const auto original = endpoint->requests.front().target;
  ++endpoint->backendGeneration;
  endpoint->value = 77;
  endpoint->version = 1;
  const auto old = endpoint->accept(0);
  CHECK(old.status == EditStatus::CLOSED);
  CHECK(take(fixture.edit.applyReply(old)));
  CHECK(endpoint->value == 77);
  CHECK(*take(field.snapshot(original)).draft == 10);
  auto foreign = old;
  ++foreign.target.sourceSession.instance;
  CHECK_FALSE(take(fixture.edit.applyReply(foreign)));
  CHECK_FALSE(editReceiptMatches(EditTarget{}, EditReceipt{}));
}

TEST_CASE("non-atomic commit preserves accepted field receipt when a later endpoint fails") {
  Fixture fixture;
  auto endpoint = std::make_shared<DeferredIntEndpoint>();
  auto age = fixture.edit.field("remote.partial-age", &Contact::age, endpoint);
  take((fixture.nameEditor.text() = "accepted name").result());
  take(age.set(10));
  endpoint->submitFailure = TreeError::NO_MEMORY;
  CHECK_FALSE(fixture.edit.commit());
  const auto result = fixture.edit.lastResult();
  REQUIRE(result);
  CHECK_FALSE(result->atomic);
  CHECK(std::ranges::any_of(result->fields, [](const auto &value) {
    return value.status == EditStatus::ACCEPTED;
  }));
  CHECK(result->fields.back().error == TreeError::NO_MEMORY);
  CHECK(fixture.value({1, 1}).name == "accepted name");
  CHECK(*take(age.snapshot()).draft == 10);
}

TEST_CASE("completion checks lifetime after validator closes the source") {
  Fixture *active{};
  ui::LineEditConfig config;
  config.validate = [&](std::string_view) {
    if (active) {
      take(active->scene->close());
    }
    return true;
  };
  Fixture fixture(EditMode::ON_COMPLETION, std::move(config));
  take((fixture.nameEditor.text() = "retained").result());
  const auto original = take(fixture.name.snapshot()).target;
  auto navigation = take(fixture.navigation.managed());
  active = &fixture;
  CHECK_FALSE(navigation.navigate({2, 1}));
  CHECK(*take(fixture.name.snapshot(original)).draft == "retained");
}

TEST_CASE("a rejected serialized request releases its slot for newer immediate input") {
  Fixture fixture(EditMode::IMMEDIATE);
  auto endpoint = std::make_shared<DeferredIntEndpoint>(false);
  auto field = fixture.edit.field("remote.rejected-age", &Contact::age, endpoint);
  take(field.set(10));
  take(field.set(20));
  REQUIRE(endpoint->requests.size() == 1);
  CHECK(take(fixture.edit.applyReply({endpoint->requests.front().target, EditStatus::REJECTED})));
  REQUIRE(endpoint->requests.size() == 2);
  CHECK(take(fixture.edit.applyReply(endpoint->accept(1))));
  CHECK(endpoint->value == 20);
}

TEST_CASE("typed string set before paint updates only its pre-admitted current payload") {
  Fixture fixture;
  fixture.go({2, 1});
  take(fixture.name.set("typed B draft"));
  CHECK(fixture.text() == "A");
  fixture.paint();
  CHECK(fixture.text() == "typed B draft");
  take(fixture.edit.commit().result());
  CHECK(fixture.value({1, 1}).name == "A");
  CHECK(fixture.value({2, 1}).name == "typed B draft");
}

TEST_CASE("discard releases its retained field slot on the same current record") {
  Fixture fixture(EditMode::EXPLICIT, {}, {.maxRetainedFields = 1});
  auto age = fixture.edit.field("discard.age", &Contact::age);
  take(age.set(10));
  take(age.discard(take(age.snapshot()).target));
  CHECK(take(age.set(20)) == EditStatus::DIRTY);
}

TEST_CASE("observed external version fences late own acceptance and retires only its pending slot") {
  struct Endpoint final : EditEndpoint<std::string> {
    EditCapabilities capabilities() const noexcept override {
      return {true, true, false};
    }

    std::expected<EditRecord<std::string>, TreeError> read(ModelResource, RowId) override {
      if (pendingRead) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      return EditRecord<std::string>{value, version};
    }

    std::expected<EditReceipt, TreeError> submit(EditRequest<std::string> request) override {
      requests.push_back(std::move(request));
      return EditReceipt{requests.back().target, EditStatus::PENDING};
    }

    std::expected<std::size_t, TreeError> ownedBytes(const std::string &candidate) const override {
      return sizeof(candidate) + candidate.capacity() + 1;
    }

    EditReceipt accept(std::size_t index) {
      const auto &request = requests.at(index);
      if (request.target.baseVersion != version) {
        return {request.target, EditStatus::CONFLICT};
      }
      value = request.value;
      return {request.target, EditStatus::ACCEPTED, ++version};
    }

    std::shared_ptr<const std::string> value = std::make_shared<const std::string>("server");
    std::uint64_t version{1};
    bool pendingRead{};
    std::vector<EditRequest<std::string>> requests;
  };

  Fixture fixture(EditMode::IMMEDIATE);
  auto endpoint = std::make_shared<Endpoint>();
  auto field = fixture.edit.field("remote.external-name", &Contact::name, endpoint);
  ui::Ref<LineEdit> editor;
  take(fixture.mount(ui::lineEdit(editor, field)));
  take((editor.text() = "pending draft").result());
  REQUIRE(endpoint->requests.size() == 1);
  const auto target = endpoint->requests.front().target;
  endpoint->pendingRead = true;
  take(fixture.scene->prepare(*fixture.app, {60, 12}));
  CHECK(take(editor.text().get()) == "pending draft");
  CHECK(*take(field.snapshot(target)).draft == "pending draft");
  endpoint->pendingRead = false;
  const auto old = endpoint->accept(0);
  endpoint->value = std::make_shared<const std::string>("external");
  ++endpoint->version;
  take(fixture.scene->prepare(*fixture.app, {60, 12}));
  CHECK(*take(field.snapshot(target)).accepted == "external");
  CHECK_FALSE(take(fixture.edit.applyReply(old)));
  const auto retained = take(field.snapshot(target));
  REQUIRE(retained.draft);
  CHECK(*retained.draft == "pending draft");
  CHECK(*retained.accepted == "external");
  CHECK(retained.status == EditStatus::CONFLICT);
  take(fixture.edit.cancel().result());
  take(field.set("new draft"));
  REQUIRE(endpoint->requests.size() == 2);
  CHECK_FALSE(take(fixture.edit.applyReply(old)));
  take(field.set("newest draft"));
  REQUIRE(endpoint->requests.size() == 2);
  CHECK(take(fixture.edit.applyReply(endpoint->accept(1))));
  REQUIRE(endpoint->requests.size() == 3);
  CHECK(take(fixture.edit.applyReply(endpoint->accept(2))));
  CHECK(*endpoint->value == "newest draft");
  CHECK_FALSE(take(field.snapshot()).draft);
}

class CallbackStringEndpoint final : public EditEndpoint<std::string> {
public:
  EditCapabilities capabilities() const noexcept override {
    return {true, true, false};
  }

  std::expected<EditRecord<std::string>, TreeError> read(ModelResource, RowId) override {
    auto copy = std::make_shared<const std::string>(backend);
    lastRead = copy;
    return EditRecord<std::string>{std::move(copy), version};
  }

  std::expected<EditReceipt, TreeError> submit(EditRequest<std::string> request) override {
    requests.push_back(std::move(request));
    if (pending) {
      return EditReceipt{requests.back().target, EditStatus::PENDING};
    }
    if (!accepting) {
      return EditReceipt{requests.back().target, EditStatus::REJECTED};
    }
    return accept(requests.size() - 1);
  }

  std::expected<std::size_t, TreeError> ownedBytes(const std::string &value) const override {
    const auto charge = sizeof(value) + value.capacity() + 1;
    const auto callback = onBytes;
    if (callback) {
      callback();
    }
    return charge;
  }

  EditReceipt accept(std::size_t index) {
    const auto &request = requests.at(index);
    if (request.target.baseVersion != version) {
      return {request.target, EditStatus::CONFLICT};
    }
    backend = *request.value;
    return {request.target, EditStatus::ACCEPTED, ++version};
  }

  std::string backend{"server"};
  std::uint64_t version{1};
  bool accepting{};
  bool pending{};
  std::weak_ptr<const std::string> lastRead;
  mutable std::function<void()> onBytes;
  std::vector<EditRequest<std::string>> requests;
};

TEST_CASE("routed Escape clears rejected session input before completion navigation") {
  Fixture fixture(EditMode::ON_COMPLETION);
  auto endpoint = std::make_shared<CallbackStringEndpoint>();
  auto field = fixture.edit.field("escape.name", &Contact::name, endpoint);
  ui::Ref<LineEdit> editor;
  take(fixture.mount(ui::lineEdit(editor, field)));
  take(fixture.app->router().setFocus(editor.node()));
  take(fixture.app->router().dispatch(TextInputEvent{U'X'}));
  take(fixture.app->router().invoke(editor.node(), ActionEvent{Action::ACTIVATE}));
  REQUIRE(endpoint->requests.size() == 1);
  REQUIRE(take(field.snapshot()).draft);
  take(fixture.app->router().invoke(editor.node(), ActionEvent{Action::CANCEL}));
  CHECK(take(editor.text().get()) == "server");
  CHECK_FALSE(take(field.snapshot()).draft);
  endpoint->accepting = true;
  fixture.go({2, 1});
  CHECK(endpoint->requests.size() == 1);
  CHECK(endpoint->backend == "server");
}

TEST_CASE("routed Cancel keeps admitted pending work without resubmission or false text revision") {
  Fixture fixture(EditMode::ON_COMPLETION);
  auto endpoint = std::make_shared<CallbackStringEndpoint>();
  endpoint->pending = true;
  auto field = fixture.edit.field("escape.pending-name", &Contact::name, endpoint);
  ui::Ref<LineEdit> editor;
  take(fixture.mount(ui::lineEdit(editor, field)));
  take(fixture.app->router().setFocus(editor.node()));
  take(fixture.app->router().dispatch(TextInputEvent{U'X'}));
  take(fixture.app->router().invoke(editor.node(), ActionEvent{Action::ACTIVATE}));
  REQUIRE(endpoint->requests.size() == 1);
  take(fixture.app->router().invoke(editor.node(), ActionEvent{Action::CANCEL}));
  CHECK(take(editor.text().get()) == "server");
  CHECK_FALSE(take(field.snapshot()).draft);
  CHECK(take(fixture.edit.applyReply(endpoint->accept(0))));
  take(fixture.scene->prepare(*fixture.app, {60, 12}));
  CHECK(take(editor.text().get()) == "serverX");
  CHECK_FALSE(take(field.snapshot()).draft);
  fixture.go({2, 1});
  CHECK(endpoint->requests.size() == 1);
}

TEST_CASE("ownedBytes reentry cannot retire its borrowed accepted owner or dirty input") {
  Fixture fixture;
  auto endpoint = std::make_shared<CallbackStringEndpoint>();
  auto field = fixture.edit.field("reentrant.name", &Contact::name, endpoint);
  take(field.set("kept"));
  const auto target = take(field.snapshot()).target;
  const auto borrowed = endpoint->lastRead;
  bool called{};
  bool ownerAlive{};
  std::optional<std::expected<void, TreeError>> discarded;
  endpoint->onBytes = [&] {
    endpoint->onBytes = {};
    called = true;
    discarded = field.discard(target);
    ownerAlive = !borrowed.expired();
  };
  const auto changed = field.set("candidate");
  CHECK(called);
  CHECK(ownerAlive);
  REQUIRE(discarded);
  CHECK_FALSE(*discarded);
  if (!*discarded) {
    CHECK(discarded->error() == TreeError::WOULD_BLOCK);
  }
  REQUIRE(changed);
  REQUIRE(take(field.snapshot()).draft);
  CHECK(*take(field.snapshot()).draft == "candidate");
}

TEST_CASE("ownedBytes routed cursor callback cannot publish an invalidated prepared history") {
  Fixture fixture;
  auto endpoint = std::make_shared<CallbackStringEndpoint>();
  auto field = fixture.edit.field("reentrant.history-name", &Contact::name, endpoint);
  ui::Ref<LineEdit> first;
  ui::Ref<LineEdit> second;
  take(fixture.mount(ui::lineEdit(first, field)));
  take(fixture.mount(ui::lineEdit(second, field)));
  take((first.text() = "kept").result());
  CHECK(first.canUndo());
  int calls{};
  bool moved{};
  endpoint->onBytes = [&] {
    // One source refresh precedes the existing prepared-history admission call.
    if (++calls == 3) {
      endpoint->onBytes = {};
      take(fixture.app->router().invoke(second.node(), ActionEvent{Action::MOVE_LEFT}));
      moved = true;
    }
  };
  const auto changed = first.text() = "replacement";
  CHECK(moved);
  CHECK_FALSE(changed.result());
  CHECK(take(first.text().get()) == "kept");
  CHECK(first.canUndo());
  take(first.undo().result());
  CHECK(take(first.text().get()) == "server");
  CHECK(endpoint->requests.empty());
}

TEST_CASE("temporary edit-session mounts prune only expired navigation participants") {
  Fixture fixture;
  auto keeper = ui::edit(fixture.navigation);
  auto keeperField = keeper.field("keeper.name", &Contact::name);
  const auto keeperNode = take(fixture.mount(ui::lineEdit(keeperField)));
  for (int iteration = 0; iteration < 64; ++iteration) {
    {
      auto temporary = ui::edit(fixture.navigation);
      const auto node = take(fixture.mount(ui::lineEdit(temporary.field(&Contact::country))));
      take(fixture.scene->remove(node));
    }
    take(fixture.app->scheduler().dispatch());
  }
  const auto record = take(ui::detail::EditAccess::context(take(fixture.navigation.managed())));
  REQUIRE(record->editors);
  // Deferred observer retirement may still own a removed widget's participant.
  // Hold every live registration and verify the next admission prunes only expired ones.
  std::vector<std::shared_ptr<detail::EditNavigationParticipant>> live;
  for (const auto &weak : *record->editors) {
    if (auto participant = weak.lock()) {
      live.push_back(std::move(participant));
    }
  }
  REQUIRE(live.size() >= 2);
  CHECK(live[0] == fixture.name.state_);
  CHECK(live[1] == keeperField.state_);
  auto admitted = ui::edit(fixture.navigation);
  auto admittedField = admitted.field(&Contact::age);
  take(admittedField.snapshot());
  REQUIRE(record->editors->size() == live.size() + 1);
  for (std::size_t index = 0; index < live.size(); ++index) {
    CHECK((*record->editors)[index].lock() == live[index]);
  }
  CHECK(record->editors->back().lock() == admittedField.state_);
  CHECK(std::ranges::none_of(*record->editors, [](const auto &weak) {
    return weak.expired();
  }));
  fixture.go({2, 1});
  CHECK(take(fixture.navigation.currentRow()) == RowId{2, 1});
  take(fixture.scene->remove(keeperNode));
}

TEST_CASE("editor selection keeps first tie and first validated priority") {
  int target{};
  auto first = std::make_shared<detail::EditTextBinding>();
  auto second = std::make_shared<detail::EditTextBinding>();
  auto validated = std::make_shared<detail::EditTextBinding>();
  auto laterValidated = std::make_shared<detail::EditTextBinding>();
  for (const auto &binding : {first, second, validated, laterValidated}) {
    binding->displayedEntry = &target;
  }
  validated->validatedByEditor = true;
  laterValidated->validatedByEditor = true;
  std::vector<std::weak_ptr<detail::EditTextBinding>> bindings{first, second};
  CHECK(ui::detail::preferredEditBinding(bindings, first));
  CHECK_FALSE(ui::detail::preferredEditBinding(bindings, second));
  bindings.push_back(validated);
  bindings.push_back(laterValidated);
  CHECK(ui::detail::preferredEditBinding(bindings, validated));
  CHECK_FALSE(ui::detail::preferredEditBinding(bindings, laterValidated));
  validated->closed = true;
  CHECK(ui::detail::preferredEditBinding(bindings, laterValidated));
  laterValidated.reset();
  CHECK(ui::detail::preferredEditBinding(bindings, first));
}

TEST_CASE("history returns with the record and external update disables stale Undo") {
  Fixture fixture(EditMode::IMMEDIATE);
  take((fixture.nameEditor.text() += "x").result());
  fixture.go({2, 1});
  fixture.paint();
  CHECK_FALSE(fixture.nameEditor.canUndo());
  fixture.go({1, 1});
  fixture.paint();
  take(fixture.nameEditor.undo().result());
  CHECK(fixture.value({1, 1}).name == "A");
  take(fixture.nameEditor.redo().result());
  CHECK(fixture.value({1, 1}).name == "Ax");
  take(fixture.model.update({1, 1}, Contact{"external", "AA", 1}));
  CHECK_FALSE(fixture.nameEditor.canUndo());
  (void)fixture.nameEditor.undo();
  CHECK(fixture.value({1, 1}).name == "external");
}

TEST_CASE("closed endpoint preserves target-qualified draft and explicit discard") {
  Fixture fixture;
  auto target = take(fixture.name.snapshot()).target;
  take((fixture.nameEditor.text() = "retained after close").result());
  take(fixture.scene->close());
  CHECK(*take(fixture.name.snapshot(target)).draft == "retained after close");
  CHECK_FALSE(fixture.edit.commit());
  take(fixture.name.discard(target));
  CHECK_FALSE(fixture.name.snapshot(target));
}

TEST_CASE("empty fields and malformed schema keys report checked failure") {
  ui::EditField<Contact, std::string> empty;
  CHECK_FALSE(empty.snapshot());
  CHECK_FALSE(empty.set("value"));
  CHECK_FALSE(empty.validate({}).snapshot());
  Fixture fixture;
  auto nullMember = fixture.edit.field(static_cast<std::string Contact::*>(nullptr));
  CHECK_FALSE(nullMember.snapshot());
  auto emptyKey = fixture.edit.field("", &Contact::name);
  CHECK_FALSE(emptyKey.set("value"));
  auto wrongStringSchema = fixture.edit.field("contact.name", &Contact::country);
  CHECK_FALSE(wrongStringSchema.snapshot());
  auto wrongSchema = fixture.edit.field("contact.name", &Contact::age);
  CHECK_FALSE(wrongSchema.snapshot());
}
} // namespace

namespace {
struct ReadyTerminal final : ApplicationReadyTerminal {
  std::optional<InputEvent> input;
  std::size_t inputs{};
  std::string pending;

  TerminalRead poll(std::stop_token) override {
    TerminalRead result;
    if (input) {
      result.events.push_back(std::move(*input));
      input.reset();
      ++inputs;
    } else {
      result.status = TerminalStatus::DEADLINE;
    }
    return result;
  }

  std::expected<CellSize, TerminalError> size() const override {
    return CellSize{60, 12};
  }

  std::expected<void, TerminalError> submit(std::string bytes) override {
    if (!pending.empty()) {
      return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_PENDING});
    }
    pending = std::move(bytes);
    return {};
  }

  TerminalWrite flushReady(std::stop_token) override {
    const auto bytes = pending.size();
    pending.clear();
    return {TerminalStatus::PROGRESS, bytes, 0, {}};
  }

  std::expected<void, TerminalError> close() noexcept override {
    return {};
  }
};

struct ActiveSession {
  Application &app;
  bool started{};

  ~ActiveSession() {
    if (started) {
      app.requestStop();
      (void)app.step();
    }
  }
};

TEST_CASE("Save business rejection stays recoverable through the actual managed button callback") {
  Fixture fixture(EditMode::EXPLICIT, {.validate = [](std::string_view value) {
                    return !value.empty();
                  }});
  ui::Ref<Button> save;
  take(fixture.mount(ui::button("Save", [edit = fixture.edit] {
                       return edit.commit();
                     }).reference(save)));
  take((fixture.nameEditor.text() = "").result());
  take(fixture.app->router().setFocus(save.node()));
  ReadyTerminal terminal;
  ActiveSession cleanup{*fixture.app};
  take(fixture.app->start(*fixture.scene, terminal));
  cleanup.started = true;
  CHECK_FALSE(take(fixture.app->step()).finished);
  terminal.input = KeyEvent{InputKey::ENTER};
  CHECK_FALSE(take(fixture.app->step()).finished);
  CHECK(fixture.value({1, 1}).name == "A");
  CHECK_FALSE(take(fixture.scene->diagnostics()).errors.empty());
  take((fixture.nameEditor.text() = "fixed").result());
  terminal.input = KeyEvent{InputKey::ENTER};
  CHECK_FALSE(take(fixture.app->step()).finished);
  CHECK(fixture.value({1, 1}).name == "fixed");
  CHECK(terminal.inputs == 2);
}

TEST_CASE("keyboard retained-budget refusal keeps the application and existing draft alive") {
  Fixture fixture;
  take((fixture.nameEditor.text() = "keep").result());
  ReadyTerminal terminal;
  ActiveSession cleanup{*fixture.app};
  take(fixture.app->start(*fixture.scene, terminal));
  cleanup.started = true;
  CHECK_FALSE(take(fixture.app->step()).finished);
  fixture.name.state_->options.maxRetainedBytes = fixture.name.state_->bytes;
  terminal.input = TextInputEvent{U'x'};
  auto refused = fixture.app->step();
  REQUIRE(refused);
  CHECK_FALSE(refused->finished);
  CHECK(fixture.text() == "keep");
  CHECK(take(fixture.navigation.currentRow()) == RowId{1, 1});
  CHECK(fixture.value({1, 1}).name == "A");
  if (!refused->finished) {
    fixture.name.state_->options.maxRetainedBytes += 20000;
    terminal.input = TextInputEvent{U'y'};
    CHECK_FALSE(take(fixture.app->step()).finished);
    CHECK(fixture.text() == "keepy");
  }
}

TEST_CASE("backend NODE_LIMIT remains a technical failure rather than retention refusal") {
  struct FailingEndpoint final : EditEndpoint<std::string> {
    EditCapabilities capabilities() const noexcept override {
      return {true, true};
    }

    std::expected<EditRecord<std::string>, TreeError> read(ModelResource, RowId) override {
      return EditRecord<std::string>{std::make_shared<const std::string>("backend"), 1};
    }

    std::expected<EditReceipt, TreeError> submit(EditRequest<std::string>) override {
      return std::unexpected(TreeError::NODE_LIMIT);
    }

    std::expected<std::size_t, TreeError> ownedBytes(const std::string &value) const override {
      return sizeof(value) + value.capacity() + 1;
    }
  };

  Fixture fixture(EditMode::IMMEDIATE);
  auto field = fixture.edit.field("technical.name", &Contact::name, std::make_shared<FailingEndpoint>());
  ui::Ref<LineEdit> editor;
  take(fixture.mount(ui::lineEdit(editor, field)));
  take(fixture.app->router().setFocus(editor.node()));
  ReadyTerminal terminal;
  ActiveSession cleanup{*fixture.app};
  take(fixture.app->start(*fixture.scene, terminal));
  cleanup.started = true;
  CHECK_FALSE(take(fixture.app->step()).finished);
  terminal.input = TextInputEvent{U'x'};
  CHECK(take(fixture.app->step()).finished);
}

TEST_CASE("external growth budget refusal preserves the running editor and honest accepted version") {
  struct Endpoint final : EditEndpoint<std::string> {
    EditCapabilities capabilities() const noexcept override {
      return {true, true, false};
    }

    std::expected<EditRecord<std::string>, TreeError> read(ModelResource, RowId) override {
      if (readFailure) {
        return std::unexpected(*readFailure);
      }
      return EditRecord<std::string>{value, version};
    }

    std::expected<EditReceipt, TreeError> submit(EditRequest<std::string> request) override {
      requests.push_back(std::move(request));
      return EditReceipt{requests.back().target, EditStatus::PENDING};
    }

    std::expected<std::size_t, TreeError> ownedBytes(const std::string &candidate) const override {
      return sizeof(candidate) + candidate.capacity() + 1;
    }

    EditReceipt accept(std::size_t index) {
      const auto &request = requests.at(index);
      if (request.target.baseVersion != version) {
        return {request.target, EditStatus::CONFLICT};
      }
      value = request.value;
      return {request.target, EditStatus::ACCEPTED, ++version};
    }

    std::shared_ptr<const std::string> value = std::make_shared<const std::string>("server");
    std::uint64_t version{1};
    std::optional<TreeError> readFailure;
    std::vector<EditRequest<std::string>> requests;
  };

  SchedulerTime now{1};
  constexpr SchedulerTime FRAME_ADVANCE = 100;
  SchedulerClock clock{[](void *value) noexcept {
                         return *static_cast<SchedulerTime *>(value);
                       },
                       &now};
  Fixture fixture(EditMode::IMMEDIATE,
                  {.validate =
                     [](std::string_view) {
                       return false;
                     }},
                  {}, nullptr, clock);
  take((fixture.nameEditor.text() = "kept sibling").result());
  auto endpoint = std::make_shared<Endpoint>();
  auto field = fixture.edit.field("remote.growing-name", &Contact::name, endpoint);
  ui::Ref<LineEdit> editor;
  take(fixture.mount(ui::lineEdit(editor, field)));
  take((editor.text() = "owned pending draft").result());
  REQUIRE(endpoint->requests.size() == 1);
  const auto target = endpoint->requests.front().target;
  auto entry = std::dynamic_pointer_cast<ui::detail::EditEntry<Contact, std::string>>(field.state_->entries.back());
  REQUIRE(entry);
  ReadyTerminal terminal;
  ActiveSession cleanup{*fixture.app};
  take(fixture.app->start(*fixture.scene, terminal));
  cleanup.started = true;
  CHECK_FALSE(take(fixture.app->step()).finished);
  const auto old = endpoint->accept(0);
  // The backend value fits the editor's 4096-byte limit, but not retained credit.
  field.state_->options.maxRetainedBytes = field.state_->bytes + 2048;
  endpoint->value = std::make_shared<const std::string>(3000, 'x');
  ++endpoint->version;
  take(fixture.app->tree().invalidate(editor.node(), Invalidation::LAYOUT));
  now += FRAME_ADVANCE;
  const auto refreshed = fixture.app->step();
  REQUIRE(refreshed);
  CHECK_FALSE(refreshed->finished);
  if (refreshed->finished) {
    return;
  }
  CHECK(take(editor.text().get()) == "owned pending draft");
  CHECK(fixture.text() == "kept sibling");
  CHECK(*take(field.snapshot(target)).accepted == "server");
  CHECK(entry->textSnapshot->revision == 1);
  CHECK(entry->target.baseVersion == 1);
  CHECK_FALSE(take(fixture.edit.applyReply(old)));
  REQUIRE(take(field.snapshot(target)).draft);
  CHECK(*take(field.snapshot(target)).draft == "owned pending draft");
  CHECK(*take(field.snapshot(target)).accepted == "server");
  CHECK(take(field.snapshot(target)).status == EditStatus::CONFLICT);
  // Routed Escape must preflight accepted-data admission before resetting input/history.
  take(fixture.app->router().invoke(editor.node(), ActionEvent{Action::CANCEL}));
  CHECK(take(editor.text().get()) == "owned pending draft");
  CHECK(fixture.text() == "kept sibling");
  REQUIRE(take(field.snapshot(target)).draft);
  CHECK(*take(field.snapshot(target)).draft == "owned pending draft");
  CHECK(entry->textSnapshot->revision == 1);
  CHECK(entry->target.baseVersion == 1);
  CHECK_FALSE(take(fixture.app->step()).finished);
  auto canceled = fixture.edit.cancel().result();
  REQUIRE_FALSE(canceled);
  CHECK(canceled.error() == TreeError::NODE_LIMIT);
  CHECK(take(editor.text().get()) == "owned pending draft");
  CHECK(fixture.text() == "kept sibling");
  REQUIRE(take(field.snapshot(target)).draft);
  CHECK(*take(field.snapshot(target)).draft == "owned pending draft");
  CHECK(entry->textSnapshot->revision == 1);
  CHECK(entry->target.baseVersion == 1);
  CHECK_FALSE(take(fixture.app->step()).finished);
  take(field.set("new draft"));
  REQUIRE(endpoint->requests.size() == 2);
  CHECK_FALSE(take(fixture.edit.applyReply(old)));
  take(field.set("newest draft"));
  REQUIRE(endpoint->requests.size() == 2);
  // Fresh accepted data must be admitted before Cancel can clear input.
  field.state_->options.maxRetainedBytes = 4 * 1024 * 1024;
  take(fixture.edit.cancel().result());
  CHECK(*take(field.snapshot(target)).accepted == std::string(3000, 'x'));
  CHECK(entry->textSnapshot->revision == 3);
  CHECK(entry->target.baseVersion == 3);
  CHECK(take(editor.text().get()) == std::string(3000, 'x'));
  CHECK_FALSE(take(field.snapshot(target)).draft);
  CHECK(take(fixture.edit.applyReply(endpoint->accept(1))));
  take(field.set("accepted next"));
  REQUIRE(endpoint->requests.size() == 3);
  CHECK_FALSE(take(fixture.edit.applyReply(old)));
  take(field.set("newest next"));
  REQUIRE(endpoint->requests.size() == 3);
  CHECK(take(fixture.edit.applyReply(endpoint->accept(2))));
  REQUIRE(endpoint->requests.size() == 4);
  CHECK(take(fixture.edit.applyReply(endpoint->accept(3))));
  CHECK(*endpoint->value == "newest next");
  CHECK_FALSE(take(field.snapshot(target)).draft);
  // The same error from the backend is not a local retention refusal.
  endpoint->readFailure = TreeError::NODE_LIMIT;
  take(fixture.app->tree().invalidate(editor.node(), Invalidation::LAYOUT));
  now += FRAME_ADVANCE;
  const auto technical = fixture.app->step();
  REQUIRE_FALSE(technical);
  const auto *cause = std::get_if<TreeError>(&technical.error().cause);
  REQUIRE(cause);
  CHECK(*cause == TreeError::NODE_LIMIT);
}

void navigationVetoKeepsAppAlive(bool tableView) {
  Fixture fixture(EditMode::ON_COMPLETION, {.validate = [](std::string_view value) {
                    return !value.empty();
                  }});
  ui::Ref<VirtualTable<Contact>> table;
  Node<Widget> view = fixture.list.node();
  if (tableView) {
    take(fixture.mount(
      ui::virtualTable(fixture.navigation, tableColumn("Name", &Contact::name, 12)).height(3).reference(table)));
    view = table.node();
    fixture.paint();
  }
  take((fixture.nameEditor.text() = "").result());
  take(fixture.app->router().setFocus(view));
  ReadyTerminal terminal;
  ActiveSession cleanup{*fixture.app};
  take(fixture.app->start(*fixture.scene, terminal));
  cleanup.started = true;
  CHECK_FALSE(take(fixture.app->step()).finished);
  terminal.input = KeyEvent{InputKey::DOWN};
  CHECK_FALSE(take(fixture.app->step()).finished);
  CHECK(take(fixture.navigation.currentRow()) == RowId{1, 1});
  CHECK(fixture.text().empty());
  CHECK_FALSE(take(fixture.app->step()).finished);
  CHECK(terminal.inputs == 1);
}

TEST_CASE("native list completion veto preserves a live application") {
  navigationVetoKeepsAppAlive(false);
}

TEST_CASE("native table completion veto preserves a live application") {
  navigationVetoKeepsAppAlive(true);
}

TEST_CASE("pristine visits do not exhaust the retained-field budget") {
  Fixture fixture(EditMode::EXPLICIT, {}, {.maxRetainedFields = 1});
  for (std::uint64_t index = 2; index < 22; ++index) {
    take(fixture.model.insert(index, Contact{std::to_string(index), "country", static_cast<int>(index)}));
  }
  for (std::uint64_t row = 1; row <= 22; ++row) {
    fixture.go({row, 1});
    fixture.paint();
    CHECK(take(fixture.navigation.currentRow()) == RowId{row, 1});
  }
  CHECK(fixture.name.state_->entries.size() <= 4);
  CHECK(ui::detail::EditAccess::sequence(fixture.model)->editVersions->size() <= 2);
}

TEST_CASE("retained field capacity refuses the next dirty input before replacing clean text") {
  Fixture fixture(EditMode::EXPLICIT, {}, {.maxRetainedFields = 1});
  const auto target = take(fixture.name.snapshot()).target;
  take((fixture.nameEditor.text() = "keep A").result());
  fixture.go({2, 1});
  fixture.paint();
  CHECK_FALSE((fixture.nameEditor.text() = "refused B").result());
  CHECK(fixture.text() == "B");
  CHECK(*take(fixture.name.snapshot(target)).draft == "keep A");
}

TEST_CASE("checked allocation failure and oversized charge preserve the previous typed draft") {
  Fixture fixture;
  auto age = fixture.edit.field("contact.age", &Contact::age);
  take(age.set(3));
  editAllocationsBeforeFailure = 0;
  const auto failed = age.set(42);
  editAllocationsBeforeFailure = -1;
  REQUIRE_FALSE(failed);
  CHECK(failed.error() == TreeError::NO_MEMORY);
  CHECK(*take(age.snapshot()).draft == 3);
  auto endpoint = std::make_shared<DeferredIntEndpoint>();
  auto remote = fixture.edit.field("huge.age", &Contact::age, endpoint);
  take(remote.set(8));
  endpoint->chargeOverride = std::numeric_limits<std::size_t>::max();
  editAllocationsBeforeFailure = 0;
  const auto tooLarge = remote.set(9);
  editAllocationsBeforeFailure = -1;
  REQUIRE_FALSE(tooLarge);
  CHECK(tooLarge.error() == TreeError::NODE_LIMIT);
  CHECK(*take(remote.snapshot()).draft == 8);
}

TEST_CASE("incoming text capacity is admitted before user navigation") {
  Fixture fixture;
  take((fixture.nameEditor.text() = "owned dirty input").result());
  const auto held = take(fixture.name.snapshot()).target;
  take(fixture.model.update({2, 1}, Contact{std::string(3000, 'b'), "BB", 2}));
  fixture.name.state_->options.maxRetainedBytes = fixture.name.state_->bytes + 20000;
  auto moved = take(fixture.navigation.managed()).navigate({2, 1});
  REQUIRE_FALSE(moved);
  CHECK(moved.error() == TreeError::NODE_LIMIT);
  CHECK(take(fixture.navigation.currentRow()) == RowId{1, 1});
  CHECK(*take(fixture.name.snapshot(held)).draft == "owned dirty input");
}

TEST_CASE("forced endpoint close retains an already-owned draft at exhausted admission credit") {
  Fixture fixture;
  const auto target = take(fixture.name.snapshot()).target;
  take((fixture.nameEditor.text() = std::string(1024, 'x')).result());
  fixture.name.state_->options.maxRetainedBytes = fixture.name.state_->bytes;
  take(fixture.scene->close());
  CHECK(*take(fixture.name.snapshot(target)).draft == std::string(1024, 'x'));
  take(fixture.name.discard(target));
  CHECK_FALSE(fixture.name.snapshot(target));
}

TEST_CASE("deferred node removal and immediate remount use the same complete draft payload") {
  Fixture fixture;
  take((fixture.nameEditor.text() = "survives removal").result());
  const auto target = take(fixture.name.snapshot()).target;
  ui::Ref<LineEdit> replacement;
  const auto previous = fixture.nameEditor.node();
  take(fixture.app->tree().read(previous, [&](const LineEdit &) {
    take(fixture.scene->remove(previous));
    take(fixture.mount(ui::lineEdit(replacement, fixture.name)));
    CHECK(take(replacement.text().get()) == "survives removal");
  }));
  CHECK(*take(fixture.name.snapshot(target)).draft == "survives removal");
  take((replacement.text() = "new draft").result());
  take(fixture.edit.commit().result());
  CHECK(fixture.value({1, 1}).name == "new draft");
}

TEST_CASE("clean retired payload remains owned through frame borrow remount and navigation") {
  Fixture fixture;
  const auto original = take(fixture.name.snapshot()).target;
  std::weak_ptr<ui::detail::EditEntryBase> old;
  for (const auto &entry : fixture.name.state_->entries) {
    if (entry->target.field == original.field) {
      old = entry;
    }
  }
  const auto node = fixture.nameEditor.node();
  take(fixture.app->tree().read(node, [&](const LineEdit &held) {
    take(fixture.scene->remove(node));
    fixture.go({2, 1});
    ui::Ref<LineEdit> replacement;
    take(fixture.mount(ui::lineEdit(replacement, fixture.name)));
    fixture.go({1, 1});
    fixture.go({2, 1});
    fixture.go({1, 1});
    ui::Ref<LineEdit> current;
    take(fixture.mount(ui::lineEdit(current, fixture.name)));
    fixture.nameEditor = current;
    CHECK_FALSE(old.expired());
    if (!old.expired()) {
      CHECK(held.text().bytes() == "A");
    }
    take((current.text() = "complete replacement").result());
    CHECK(take(current.text().get()) == "complete replacement");
    if (!old.expired()) {
      CHECK(held.text().bytes() == "complete replacement");
    }
  }));
  CHECK(*take(fixture.name.snapshot(original)).draft == "complete replacement");
}

TEST_CASE("rendered required error disappears when the record draft is canceled") {
  Fixture fixture;
  ui::Ref<FormField> form;
  take(fixture.mount(ui::formField("Required", fixture.name, {.required = true}).reference(form)));
  take((form.text() = "accepted name").result());
  take(fixture.edit.commit().result());
  take((form.text() = "").result());
  CHECK_FALSE(fixture.edit.commit());
  const auto rejected = fixture.renderCells();
  CHECK(rejected.find("Error: A value is required") != std::string::npos);
  take(fixture.edit.cancel().result());
  const auto canceled = fixture.renderCells();
  CHECK(canceled.find("accepted name") != std::string::npos);
  CHECK(canceled.find("Error:") == std::string::npos);
  CHECK(take(form.validation()).status == FieldStatus::UNCHECKED);
}

TEST_CASE("FormField validation returns with its record") {
  Fixture fixture(EditMode::IMMEDIATE);
  ui::Ref<FormField> form;
  take(fixture.mount(ui::formField("Required", fixture.name, {.required = true}).reference(form)));
  take((form.text() = "").result());
  CHECK(take(form.validation()).status == FieldStatus::INVALID);
  fixture.go({2, 1});
  fixture.paint();
  Sink sink;
  take(fixture.app->tree().frame([&] {
    take(fixture.app->tree().paint(form.editor().node(), sink));
  }));
  CHECK(take(form.validation()).status == FieldStatus::UNCHECKED);
  fixture.go({1, 1});
  fixture.paint();
  take(fixture.app->tree().frame([&] {
    take(fixture.app->tree().paint(form.editor().node(), sink));
  }));
  CHECK(take(form.validation()).status == FieldStatus::INVALID);
  CHECK(take(form.text().get()).empty());
}
} // namespace

namespace {
class ValidationExecutor final : public BackgroundExecutor {
public:
  std::vector<std::shared_ptr<BackgroundJob>> jobs;

  std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &job) override {
    jobs.emplace_back(std::move(job));
    return BackgroundTicket(jobs.back());
  }

  void service() noexcept override {
  }

  std::uint64_t capacityEpoch() const noexcept override {
    return 0;
  }

  void finish(std::size_t index) {
    jobs.at(index)->run(jobs.at(index)->stopToken());
    take(jobs.at(index)->publish());
  }
};

TEST_CASE("async FormField gate composes with retained payload and rejects the old row reply") {
  ValidationExecutor executor;
  Fixture fixture(EditMode::EXPLICIT, {}, {}, &executor);
  ui::Ref<FormField> form;
  take(fixture.mount(ui::formField("Async", fixture.name,
                                   {.validateAsync =
                                      [](std::string_view, std::stop_token) {
                                        return FieldValidation::valid("Checked");
                                      }})
                       .reference(form)));
  take((form.text() = "A pending").result());
  take(fixture.edit.commit().result());
  take(fixture.scene->afterDispatch(*fixture.app));
  REQUIRE(executor.jobs.size() == 1);
  CHECK(fixture.value({1, 1}).name == "A");
  fixture.go({2, 1});
  fixture.paint();
  Sink sink;
  take(fixture.app->tree().frame([&] {
    take(fixture.app->tree().paint(form.editor().node(), sink));
  }));
  executor.finish(0);
  take(fixture.app->scheduler().dispatch());
  take(fixture.scene->afterDispatch(*fixture.app));
  CHECK(fixture.value({1, 1}).name == "A");
  CHECK(fixture.value({2, 1}).name == "B");
  CHECK(take(form.text().get()) == "B");
  CHECK(take(form.validation()).status == FieldStatus::UNCHECKED);
}

TEST_CASE("re-enrollment keeps session sequence monotonic and rejects a retired receipt") {
  Fixture fixture(EditMode::IMMEDIATE);
  auto endpoint = std::make_shared<DeferredIntEndpoint>();
  auto field = fixture.edit.field("retired.age", &Contact::age, endpoint);
  take(field.set(10));
  const EditReceipt rejected{endpoint->requests.front().target, EditStatus::REJECTED};
  CHECK(take(fixture.edit.applyReply(rejected)));
  take(fixture.edit.cancel().result());
  fixture.go({2, 1});
  fixture.paint();
  fixture.go({1, 1});
  fixture.paint();
  take(field.set(20));
  REQUIRE(endpoint->requests.size() == 2);
  CHECK(endpoint->requests.back().target.baseVersion == rejected.target.baseVersion);
  CHECK(endpoint->requests.back().target.sequence > rejected.target.sequence);
  CHECK_FALSE(take(fixture.edit.applyReply(rejected)));
  CHECK(endpoint->value == 7);
}

class SparseReadOnlyRows final : public ModelSnapshot<Contact> {
public:
  SparseReadOnlyRows() : ModelSnapshot(1, 1000000) {
  }

  ModelRows<Contact> read(RowRange range) const override {
    ++reads;
    CHECK(range.first == 500000);
    CHECK(range.count == 1);
    return {RangeState::READY, row, {}};
  }

  RowLookup locateRow(RowId id) const override {
    return id == RowId{500001, 1} ? RowLookup{RowLookupState::FOUND, 500000} : RowLookup{RowLookupState::NOT_FOUND, 0};
  }

  mutable int reads{};
  std::array<ModelRow<Contact>, 1> row{{{{500001, 1}, {"Sparse", "country", 1}}}};
};

TEST_CASE("readonly provider reads a single current field without materializing its extent") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto rows = std::make_shared<SparseReadOnlyRows>();
  auto model = ui::model<Contact>(rows);
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation);
  auto field = edit.field(&Contact::name);
  ui::Ref<LineEdit> editor;
  take(ui::mount(*scene, ui::lineEdit(editor, field)));
  CHECK(rows->reads == 0);
  take(take(navigation.managed()).navigate({500001, 1}));
  const auto snapshot = take(field.snapshot());
  CHECK(*snapshot.accepted == "Sparse");
  CHECK(rows->reads == 1);
  CHECK(take(field.set("refused")) == EditStatus::READ_ONLY);
  CHECK(rows->reads == 1);
}

TEST_CASE("temporary cold declaration keeps the actual managed rebound write capability") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto original = ui::model(std::array{Contact{"old", "country", 1}});
  auto navigation = ui::navigation(original);
  take(ui::mount(*scene, ui::virtualList(navigation, &Contact::name)));
  auto managed = take(navigation.managed());
  {
    auto temporary = ui::model(std::array{Contact{"new", "country", 2}});
    take(ui::mount(*scene, ui::virtualList(temporary, &Contact::name)));
    take(managed.setModel(take(temporary.managed())));
  }
  take(managed.navigate({1, 1}));
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});
  auto name = edit.field(&Contact::name);
  take(name.set("changed actual source"));
  take(edit.commit().result());
  CHECK(*take(name.snapshot()).accepted == "changed actual source");
  CHECK(ui::detail::EditAccess::sequence(original)->current->rows().front().value.name == "old");
  take(scene->close());
  CHECK_FALSE(name.set("closed"));
}

TEST_CASE("readonly user text and Enter keep the actual application running and allow focus traversal") {
  ui::LineEditConfig config;
  config.options.readOnly = true;
  Fixture fixture(EditMode::EXPLICIT, std::move(config));
  ReadyTerminal terminal;
  ActiveSession cleanup{*fixture.app};

  take(fixture.app->start(*fixture.scene, terminal));
  cleanup.started = true;
  CHECK_FALSE(take(fixture.app->step()).finished);
  terminal.input = TextInputEvent{U'X'};
  CHECK_FALSE(take(fixture.app->step()).finished);
  CHECK(fixture.text() == "A");
  terminal.input = KeyEvent{InputKey::ENTER};
  CHECK_FALSE(take(fixture.app->step()).finished);
  CHECK(fixture.text() == "A");
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
  terminal.input = KeyEvent{InputKey::TAB};
  CHECK_FALSE(take(fixture.app->step()).finished);
  terminal.input = TextInputEvent{U'Y'};
  CHECK_FALSE(take(fixture.app->step()).finished);
  CHECK(take(fixture.countryEditor.text().get()) == "AAY");
}

} // namespace
