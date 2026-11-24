// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>
#include <portals/ui_model_form.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace {
constexpr std::size_t LARGE_FORM_STRING_BYTES = 128 * 1024;
thread_local bool observeLargeFormAllocations{};
thread_local std::size_t largeFormAllocations{};
} // namespace

// Observe only selected-string materialization; no allocation failure is injected.
void *operator new(std::size_t size) {
  if (observeLargeFormAllocations && size >= LARGE_FORM_STRING_BYTES) {
    ++largeFormAllocations;
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
T take(std::expected<T, E> result, std::source_location origin = std::source_location::current()) {
  INFO(std::string_view(origin.file_name()), ":", origin.line());
  const auto errorCode = [&] {
    if (!result) {
      if constexpr (std::same_as<E, TreeError>) {
        return static_cast<int>(result.error());
      } else if constexpr (std::same_as<E, ManagedError>) {
        if (const auto *cause = std::get_if<TreeError>(&result.error().cause)) {
          return static_cast<int>(*cause);
        }
      }
    }

    return -1;
  }();
  INFO("TreeError=", errorCode);
  REQUIRE(result);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct City {
  std::string name;
  int orders{};
  std::string region;
  double rate{};
  int auxiliary{};
  bool operator==(const City &) const = default;
};

struct Sink final : PaintContext {
  std::expected<void, TreeError> submit(std::span<const RenderCommand>) override {
    return {};
  }
};

void paintEditors(Application &app, ManagedScene &scene, std::initializer_list<ui::Ref<FormField>> fields) {
  // A structural publication can resolve current after editors were synchronized. The ordinary
  // next frame consumes that new position; run the same two bounded frame turns in this fixture.
  constexpr int NAVIGATION_SETTLING_FRAMES = 2;
  for (int turn = 0; turn < NAVIGATION_SETTLING_FRAMES; ++turn) {
    take(app.scheduler().dispatch());
    take(scene.afterDispatch(app));
    take(scene.prepare(app, {60, 28}));
    const auto bounds = take(CellRect::from({}, {60, 28}));
    std::optional<LayoutSnapshot> layout;
    take(app.tree().frame([&] {
      take(layoutTree(app.tree(), app.root(), bounds, bounds));
      layout = take(LayoutSnapshot::capture(app.tree(), app.root()));
    }));
    take(scene.prepareViews(app, *layout));
    Sink sink;
    take(app.tree().frame([&] {
      for (const auto &field : fields) {
        take(app.tree().paint(field.editor().node(), sink));
      }
    }));
  }
}

std::string labelText(Application &app, Node<Widget> node) {
  std::string result("", 0);
  take(app.tree().read(node, [&](const Widget &widget) {
    if (const auto *label = dynamic_cast<const Label *>(&widget)) {
      result = label->text().bytes();
    }
  }));
  for (auto child : take(app.tree().inspect(node)).children) {
    result += labelText(app, child);
  }

  return result;
}

struct NumberEndpoint final : EditEndpoint<int> {
  EditCapabilities capabilities() const noexcept override {
    return {true, true, false};
  }

  std::expected<EditRecord<int>, TreeError> read(ModelResource, RowId row) override {
    return EditRecord<int>{std::make_shared<const int>(row == RowId{1, 1} ? first : 20), version};
  }

  std::expected<EditReceipt, TreeError> submit(EditRequest<int> request) override {
    requests.push_back(std::move(request));

    return EditReceipt{requests.back().target, EditStatus::PENDING};
  }

  EditReceipt accept(std::size_t index) {
    const auto request = requests.at(index);
    first = *request.value;

    return {request.target, EditStatus::ACCEPTED, ++version};
  }

  std::expected<std::size_t, TreeError> ownedBytes(const int &) const override {
    return sizeof(int);
  }

  int first{10};
  std::uint64_t version{1};
  std::vector<EditRequest<int>> requests = std::vector<EditRequest<int>>(0);
};

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  ui::Model<City> model = ui::model(std::array{City{"Paris", 10, "Europe", 1.5}, City{"Tokyo", 20, "Asia", 2.5}});
  ui::Navigation<City> navigation = ui::navigation(model);
  ui::EditSession<City> edit;
  ui::Ref<FormField> name;
  ui::Ref<FormField> orders;
  ui::Ref<FormField> region;
  ui::Ref<FormField> rate;
  Node<Widget> content;

  explicit Fixture(EditMode mode = EditMode::EXPLICIT, std::shared_ptr<NumberEndpoint> endpoint = {}) {
    edit = ui::edit(navigation, {.mode = mode});
    if (endpoint) {
      (void)edit.field("city.orders", &City::orders, endpoint);
    }
    content = take(
      ui::mount(*scene, ui::vbox(ui::virtualList(navigation, &City::name).height(3),
                                 ui::form(edit, ui::field("City", &City::name).required().reference(name),
                                          ui::field("Orders", &City::orders)
                                            .key("city.orders")
                                            .validate([](int n) {
                                              return n >= 0;
                                            })
                                            .reference(orders),
                                          ui::field("Region (read only)", &City::region).readOnly().reference(region),
                                          ui::field("Rate", &City::rate).reference(rate)))));
  }

  void paint() {
    paintEditors(*app, *scene, {name, orders, region, rate});
  }

  void go(RowId row) {
    take(take(navigation.managed()).navigate(row));
    paint();
  }

  City value(RowId row = {1, 1}) {
    auto source = ui::detail::EditAccess::sequence(model)->current;
    auto location = source->locateRow(row);
    REQUIRE(location.state == RowLookupState::FOUND);

    return source->rows()[location.index].value;
  }
};

TEST_CASE("declarative typed form is cold and no-current editors are empty inactive") {
  Fixture fixture;
  fixture.paint();
  CHECK(take(fixture.name.text().get()).empty());
  CHECK(take(fixture.orders.text().get()).empty());
  auto result = fixture.orders.text() = "42";
  CHECK_FALSE(result.result());
  CHECK(fixture.value().orders == 10);
  fixture.go({1, 1});
  CHECK(take(fixture.name.text().get()) == "Paris");
  CHECK(take(fixture.orders.text().get()) == "10");
  CHECK(take(fixture.rate.text().get()) == "1.5");
}

TEST_CASE("invalid numeric draft and history belong to exact A B A records") {
  Fixture fixture;
  fixture.go({1, 1});
  const auto target = take(fixture.edit.field("city.orders", &City::orders).snapshot()).target;
  take((fixture.orders.text() = "12x").result());
  CHECK_FALSE(fixture.edit.commit());
  CHECK(fixture.value().orders == 10);
  CHECK(take(fixture.orders.validation()).status == FieldStatus::INVALID);
  fixture.go({2, 1});
  CHECK(take(fixture.orders.text().get()) == "20");
  take((fixture.orders.text() = "25").result());
  fixture.go({1, 1});
  CHECK(take(fixture.orders.text().get()) == "12x");
  CHECK(take(fixture.edit.field("city.orders", &City::orders).snapshot(target)).status == EditStatus::INVALID);
  take(fixture.orders.editor().undo().result());
  CHECK(take(fixture.orders.text().get()) == "10");
  take(fixture.orders.editor().redo().result());
  CHECK(take(fixture.orders.text().get()) == "12x");
  take(fixture.edit.cancel().result());
  CHECK(take(fixture.orders.text().get()) == "10");
  CHECK(take(fixture.orders.validation()).status == FieldStatus::UNCHECKED);
  fixture.go({2, 1});
  CHECK(take(fixture.orders.text().get()) == "25");
  take(fixture.edit.commit().result());
  CHECK(fixture.value({2, 1}).orders == 25);
}

TEST_CASE("required typed validation and complete numeric conversion gate publication") {
  Fixture fixture(EditMode::IMMEDIATE);
  fixture.go({1, 1});
  take((fixture.name.text() = "").result());
  CHECK(fixture.value().name == "Paris");
  CHECK(take(fixture.name.validation()).status == FieldStatus::INVALID);
  for (auto invalid : {"-1", "1tail", "999999999999999999999999999999", ""}) {
    take((fixture.orders.text() = invalid).result());
    CHECK(fixture.value().orders == 10);
    CHECK(take(fixture.orders.text().get()) == invalid);
  }
  take((fixture.orders.text() = "42").result());
  CHECK(fixture.value().orders == 42);
  take((fixture.rate.text() = "nan").result());
  CHECK(fixture.value().rate == 1.5);
  take((fixture.rate.text() = "3.25").result());
  CHECK(fixture.value().rate == 3.25);
}

TEST_CASE("readonly mapping blocks routed user input and completion mode vetoes invalid navigation") {
  Fixture fixture(EditMode::ON_COMPLETION);
  fixture.go({1, 1});
  take(fixture.app->router().setFocus(fixture.region.editor().node()));
  take(fixture.app->router().dispatch(TextInputEvent{U'X'}));
  CHECK(take(fixture.region.text().get()) == "Europe");
  take((fixture.orders.text() = "bad").result());
  CHECK_FALSE(take(fixture.navigation.managed()).navigate({2, 1}));
  CHECK(take(fixture.navigation.currentRow()) == RowId{1, 1});
  take(fixture.edit.cancel().result());
  fixture.go({2, 1});
  CHECK(take(fixture.name.text().get()) == "Tokyo");
}

TEST_CASE("typed setters fluent text input and paste share numeric draft history") {
  Fixture fixture(EditMode::IMMEDIATE);
  fixture.go({1, 1});
  take(fixture.edit.field("city.orders", &City::orders).set(23));
  CHECK(fixture.value().orders == 23);
  CHECK(take(fixture.orders.text().get()) == "23");
  take(fixture.app->router().setFocus(fixture.orders.editor().node()));
  take(fixture.app->router().dispatch(TextInputEvent{U'4'}));
  CHECK(fixture.value().orders == 234);
  take(fixture.app->router().dispatch(PasteInputEvent{"5"}));
  CHECK(fixture.value().orders == 2345);
  take(fixture.orders.editor().undo().result());
  CHECK(fixture.value().orders == 234);
}

TEST_CASE("numeric late reply cannot replace another current row and external edits conflict") {
  auto endpoint = std::make_shared<NumberEndpoint>();
  Fixture fixture(EditMode::EXPLICIT, endpoint);
  fixture.go({1, 1});
  take((fixture.orders.text() = "42").result());
  take(fixture.edit.commit().result());
  REQUIRE(endpoint->requests.size() == 1);
  fixture.go({2, 1});
  CHECK(take(fixture.orders.text().get()) == "20");
  CHECK(take(fixture.edit.applyReply(endpoint->accept(0))));
  fixture.paint();
  CHECK(take(fixture.orders.text().get()) == "20");
  CHECK_FALSE(take(fixture.edit.applyReply(endpoint->accept(0))));
  fixture.go({1, 1});
  CHECK(take(fixture.orders.text().get()) == "42");

  Fixture owned;
  owned.go({1, 1});
  take((owned.orders.text() = "99").result());
  take(owned.model.update({1, 1}, City{"external", 77, "Europe", 1.5}));
  CHECK_FALSE(owned.edit.commit());
  CHECK(owned.value().orders == 77);
  CHECK(take(owned.orders.text().get()) == "99");
  take(owned.edit.cancel().result());
  CHECK(take(owned.orders.text().get()) == "77");
}

TEST_CASE("mapping preserves prior typed validation and rejects incompatible codec owners") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::IMMEDIATE});
  auto orders = edit.field(&City::orders).validate([](int n) {
    return n < 100;
  });
  ui::Ref<FormField> field;
  const auto content = take(ui::mount(*scene, ui::form(edit, ui::field("Orders", &City::orders)
                                                               .validate([](int n) {
                                                                 return n >= 0;
                                                               })
                                                               .reference(field))));
  take(take(navigation.managed()).navigate({1, 1}));
  take(orders.set(200));
  CHECK(*take(orders.snapshot()).accepted == 10);
  auto incompatible = std::make_shared<const ui::EditTextCodec<int>>(
    ui::EditTextCodec<int>{.format = [](const int &n) -> std::expected<std::string, TreeError> {
                             return std::to_string(n);
                           },
                           .parse = [](std::string_view) -> std::expected<int, TreeError> {
                             return 7;
                           }});
  CHECK_FALSE(ui::detail::mountAt(*scene, content, ui::lineEdit(orders, incompatible), ui::ControlContext{}));
}

TEST_CASE("live captions and explicit custom owning codec use real form editors") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto caption = ui::model(std::string("Orders"));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});
  ui::Ref<FormField> field;
  take(ui::mount(
    *scene, ui::form(edit, ui::field(caption, &City::orders)
                             .required()
                             .codec({.format = [](const int &n) -> std::expected<std::string, TreeError> {
                                       return std::to_string(n) + " orders";
                                     },
                                     .parse = [](std::string_view text) -> std::expected<int, TreeError> {
                                       if (!text.ends_with(" orders")) {
                                         return std::unexpected(TreeError::INVALID_ARGUMENT);
                                       }
                                       text.remove_suffix(7);
                                       int n{};
                                       const auto result = std::from_chars(text.data(), text.data() + text.size(), n);
                                       if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
                                         return std::unexpected(TreeError::INVALID_ARGUMENT);
                                       }
                                       return n;
                                     }})
                             .reference(field))));
  take(take(navigation.managed()).navigate({1, 1}));
  paintEditors(*app, *scene, {field});
  take((field.text() = "15 orders").result());
  take(edit.commit().result());
  CHECK(*take(edit.field(&City::orders).snapshot()).accepted == 15);
  take((caption.value() = std::string("Commandes")).result());
  take(scene->prepare(*app, {60, 12}));
}

TEST_CASE("custom editor factory owns its codec and explicitly applies native readonly policy") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});
  auto codec = ui::detail::defaultFormCodec<int>();
  ui::Ref<FormField> custom;
  take(ui::mount(*scene, ui::form(edit, ui::field("Custom orders", &City::orders)
                                          .readOnly()
                                          .editor([codec](ui::EditField<City, int> field, bool readOnly) {
                                            return ui::lineEdit(std::move(field), codec).readOnly(readOnly);
                                          })
                                          .reference(custom))));
  take(take(navigation.managed()).navigate({1, 1}));
  paintEditors(*app, *scene, {custom});
  take(app->router().setFocus(custom.editor().node()));
  take(app->router().dispatch(TextInputEvent{U'X'}));
  CHECK(take(custom.text().get()) == "10");
}

class ReadOnlyCities final : public ModelSnapshot<City> {
public:
  explicit ReadOnlyCities(std::string name = "Read only") : ModelSnapshot(1, 2) {
    rows[0] = {{1, 1}, {name, 10, "Europe", 1.5}};
    rows[1] = {{2, 1}, {"Other", 20, "Asia", 2.5}};
  }

  ModelRows<City> read(RowRange range) const override {
    return {RangeState::READY, std::span(rows).subspan(range.first, range.count), {}};
  }

  RowLookup locateRow(RowId row) const override {
    return row.generation == 1 && row.value > 0 && row.value <= 2 ? RowLookup{RowLookupState::FOUND, row.value - 1}
                                                                  : RowLookup{RowLookupState::NOT_FOUND, 0};
  }

  std::array<ModelRow<City>, 2> rows;
};

TEST_CASE("readonly source fields remain readable while navigation stays usable") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto rows = std::make_shared<ReadOnlyCities>();
  auto model = ui::model<City>(rows);
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation);
  ui::Ref<FormField> orders;
  take(ui::mount(*scene, ui::form(edit, ui::field("Orders", &City::orders).reference(orders))));
  auto current = take(navigation.managed());
  take(current.navigate({1, 1}));
  CHECK(*take(edit.field(&City::orders).snapshot()).accepted == 10);
  CHECK(take(edit.field(&City::orders).set(42)) == EditStatus::READ_ONLY);
  take(current.navigate({2, 1}));
  CHECK(*take(edit.field(&City::orders).snapshot()).accepted == 20);
}

TEST_CASE("default scalar string mapping refuses oversized source before selected-string copying") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto rows = std::make_shared<ReadOnlyCities>(std::string(LARGE_FORM_STRING_BYTES, 'x'));
  auto model = ui::model<City>(rows);
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation);
  take(ui::mount(*scene, ui::form(edit, ui::field("Name", &City::name))));
  take(take(navigation.managed()).navigate({1, 1}));
  largeFormAllocations = 0;
  observeLargeFormAllocations = true;
  const auto value = edit.field(&City::name).snapshot();
  observeLargeFormAllocations = false;
  CHECK_FALSE(value);
  CHECK(value.error() == TreeError::INVALID_ARGUMENT);
  CHECK(largeFormAllocations == 0);
}

TEST_CASE("live caption survives replacement of ordinary form options") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto caption = ui::model(std::string("Orders"));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation);
  ui::Ref<FormField> field;
  take(ui::mount(
    *scene, ui::form(edit, ui::field(caption, &City::orders).options({.help = "Count"}).required().reference(field))));
  CHECK(labelText(*app, field.node()).find("Orders") != std::string::npos);
  take((caption.value() = "Заказы").result());
  take(scene->prepare(*app, {60, 28}));
  take(scene->prepare(*app, {60, 28}));
  CHECK(labelText(*app, field.node()).find("Заказы") != std::string::npos);
}

TEST_CASE("native string typed set refuses editor limit before identity formatting copy") {
  Fixture fixture;
  fixture.go({1, 1});
  const std::string oversized(LARGE_FORM_STRING_BYTES, 'x');
  largeFormAllocations = 0;
  observeLargeFormAllocations = true;
  const auto rejected = fixture.edit.field(&City::name).set(oversized);
  observeLargeFormAllocations = false;
  CHECK_FALSE(rejected);
  CHECK(largeFormAllocations == 0);
  CHECK(take(fixture.name.text().get()) == "Paris");
}

TEST_CASE("native string mapping rejects an existing explicit codec representation") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation);
  auto custom = ui::field("Custom", &City::name)
                  .codec({.format = [](const std::string &value) -> std::expected<std::string, TreeError> {
                            return value;
                          },
                          .parse = [](std::string_view value) -> std::expected<std::string, TreeError> {
                            return std::string(value);
                          }});
  const auto content = take(ui::mount(*scene, ui::form(edit, custom)));
  const auto rejected =
    ui::detail::mountAt(*scene, content, ui::form(edit, ui::field("Native", &City::name)), ui::ControlContext{});
  CHECK_FALSE(rejected);
  if (!rejected) {
    CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::INVALID_ARGUMENT);
  }
}

TEST_CASE("retained inspection propagates codec errors while preserving owning text") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});
  auto fail = std::make_shared<bool>(false);
  ui::Ref<FormField> field;
  take(ui::mount(*scene,
                 ui::form(edit, ui::field("Orders", &City::orders)
                                  .codec({.format = [](const int &value) -> std::expected<std::string, TreeError> {
                                            return std::to_string(value);
                                          },
                                          .parse = [fail](std::string_view value) -> std::expected<int, TreeError> {
                                            if (*fail) {
                                              return std::unexpected(TreeError::NO_MEMORY);
                                            }
                                            int number{};
                                            const auto parsed =
                                              std::from_chars(value.data(), value.data() + value.size(), number);
                                            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                                              return std::unexpected(TreeError::INVALID_ARGUMENT);
                                            }
                                            return number;
                                          }})
                                  .reference(field))));
  take(take(navigation.managed()).navigate({1, 1}));
  paintEditors(*app, *scene, {field});
  take((field.text() = "12").result());
  *fail = true;
  auto snapshot = edit.field(&City::orders).snapshot();
  CHECK_FALSE(snapshot);
  if (!snapshot) {
    CHECK(snapshot.error() == TreeError::NO_MEMORY);
  }
  CHECK(take(field.text().get()) == "12");
  *fail = false;
  CHECK(*take(edit.field(&City::orders).snapshot()).draft == 12);
}

TEST_CASE("ordinary action row is a direct form child using the existing layout transaction") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});
  ui::Ref<FormField> name;
  ui::Ref<FormField> orders;
  ui::Ref<Button> save;
  const auto form = take(ui::mount(*scene, ui::form(edit, ui::field("City", &City::name).required().reference(name),
                                                    ui::field("Orders", &City::orders).reference(orders),
                                                    ui::hbox(ui::button(save, "Save", [edit] {
                                                      return edit.commit();
                                                    })))));
  const auto children = take(app->tree().inspect(form)).children;
  REQUIRE(children.size() == 3);
  take(app->tree().read(children.back(), [&](const Widget &widget) {
    CHECK(dynamic_cast<const Row *>(&widget) != nullptr);
  }));
  take(take(navigation.managed()).navigate({1, 1}));
  paintEditors(*app, *scene, {name, orders});
  take((orders.text() = "17").result());
  take(app->router().invoke(save.node(), ActionEvent{Action::ACTIVATE}));
  CHECK(*take(edit.field(&City::orders).snapshot()).accepted == 17);
}

TEST_CASE("codec callback failures use checked errors and keep the accepted value") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::IMMEDIATE});
  ui::Ref<FormField> field;
  auto outcomes = std::make_shared<std::vector<LineEditResult>>(0);
  ui::FormFieldConfig config;

  config.editor.result = [outcomes](LineEditResult result) -> std::expected<void, TreeError> {
    outcomes->push_back(std::move(result));

    return {};
  };
  take(
    ui::mount(*scene, ui::form(edit, ui::field("Orders", &City::orders)
                                       .options(config)
                                       .codec({.format = [](const int &value) -> std::expected<std::string, TreeError> {
                                                 return std::expected<std::string, TreeError>(std::in_place,
                                                                                              std::to_string(value));
                                               },
                                               .parse = [](std::string_view value) -> std::expected<int, TreeError> {
                                                 if (value == "memory") {
                                                   return std::unexpected(TreeError::NO_MEMORY);
                                                 }
                                                 if (value == "throw-memory") {
                                                   throw std::bad_alloc();
                                                 }
                                                 if (value == "throw-callback") {
                                                   throw std::runtime_error("codec failed");
                                                 }
                                                 return 10;
                                               }})
                                       .reference(field))));
  take(take(navigation.managed()).navigate({1, 1}));
  paintEditors(*app, *scene, {field});
  for (auto candidate : {"memory", "throw-memory", "throw-callback"}) {
    CAPTURE(std::string_view(candidate));
    outcomes->clear();
    // Publication errors do not undo successful local draft admission.
    take((field.text() = candidate).result());
    REQUIRE_FALSE(outcomes->empty());
    const auto expected =
      std::string_view(candidate) == "throw-callback" ? TreeError::CALLBACK_FAILED : TreeError::NO_MEMORY;
    CHECK(outcomes->back().error == expected);
    CHECK(take(field.text().get()) == candidate);
    const auto checked = field.validate();
    REQUIRE_FALSE(checked);
    CHECK(std::get<TreeError>(checked.error().cause) == expected);
    CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.orders == 10);
  }
}

TEST_CASE("live required marker keeps the native caption click-to-focus route") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto caption = ui::model(std::string("Orders"));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation);
  ui::Ref<FormField> field;
  ui::Ref<Button> other;
  take(ui::mount(*scene, ui::vbox(ui::form(edit, ui::field(caption, &City::orders).required().reference(field)),
                                  ui::button(other, "Other", [] {
                                  }))));
  take(take(navigation.managed()).navigate({1, 1}));
  paintEditors(*app, *scene, {field});
  const auto heading = take(app->tree().inspect(field.node())).children.front();
  const auto marker = take(app->tree().inspect(heading)).children.at(1);
  const auto origin = take(app->tree().inspect(marker)).geometry.surface.origin();
  take(other.focus());
  take(app->router().dispatch(MouseInputEvent{origin.x, origin.y, MouseButton::LEFT, MouseAction::PRESS, {}}));
  CHECK(take(app->router().focused()) == field.editor().node());
}

TEST_CASE("malformed numeric orphan has an explicit owning exact-target text recovery path") {
  Fixture fixture;
  fixture.go({1, 1});
  auto orders = fixture.edit.field("city.orders", &City::orders);
  const auto target = take(orders.snapshot()).target;
  take((fixture.orders.text() = "12x").result());
  take(fixture.model.erase(0));
  fixture.paint();
  CHECK(take(fixture.orders.text().get()) == "20");
  const auto recovered = take(orders.draftText(target));
  REQUIRE(recovered);
  CHECK(*recovered == "12x");
  CHECK(take(fixture.orders.text().get()) == "20");
  std::optional<TreeError> otherThreadError;
  std::thread other([&] {
    const auto result = orders.draftText(target);
    if (!result) {
      otherThreadError = result.error();
    }
  });
  other.join();
  CHECK(otherThreadError == TreeError::WRONG_THREAD);
  auto unbound = fixture.edit.field(&City::auxiliary);
  const auto unboundTarget = take(unbound.snapshot()).target;
  CHECK_FALSE(take(unbound.draftText(unboundTarget)));
  take(fixture.edit.commit().result());
  CHECK(fixture.value({2, 1}).orders == 20);
  CHECK(*take(orders.draftText(target)) == "12x");
  take(orders.discard(target));
  CHECK(*recovered == "12x");
  CHECK_FALSE(orders.draftText(target));
  CHECK(fixture.value({2, 1}).orders == 20);
}

TEST_CASE("standalone form follows forced current replacement without a list view") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}, City{"B", 20, "R", 2.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});
  ui::Ref<FormField> field;
  take(ui::mount(*scene, ui::form(edit, ui::field("Orders", &City::orders).reference(field))));
  auto current = take(navigation.managed());
  take(current.navigate({1, 1}));
  paintEditors(*app, *scene, {field});
  auto orders = edit.field(&City::orders);
  const auto original = take(orders.snapshot()).target;
  take((field.text() = "12x").result());
  take(model.erase(0));
  paintEditors(*app, *scene, {field});
  const auto channel = take(current.current());
  const auto position = take(channel.snapshot())->value;
  CAPTURE(static_cast<int>(position.resolution));
  CHECK(position.resolution == PositionResolution::NEAREST);
  CHECK(take(navigation.currentRow()) == RowId{2, 1});
  CHECK(take(field.text().get()) == "20");
  const auto recovered = take(orders.draftText(original));
  REQUIRE(recovered);
  CHECK(*recovered == "12x");
  const auto replacement = take(orders.snapshot()).target;
  CHECK(replacement.row == RowId{2, 1});
  CHECK(replacement.row != original.row);
  take(edit.commit().result());
  CHECK(*take(orders.draftText(original)) == "12x");
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.orders == 20);
}

TEST_CASE("unmounted form policy does not constrain a later optional editor") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::IMMEDIATE});
  auto name = edit.field(&City::name).validate([](const std::string &value) {
    return value != "blocked";
  });
  ui::FormFieldConfig oldConfig{.required = true,
                                .validate =
                                  [](std::string_view value) {
                                    return value == "old-text" ? FieldValidation::invalid("Old text rule")
                                                               : FieldValidation::valid();
                                  },
                                .editor = {.validate = [](std::string_view value) {
                                  return value != "old-legacy";
                                }}};
  auto abandoned =
    ui::form(edit, ui::field("Unused", &City::name).options(oldConfig).validate([](const std::string &value) {
      return value != "old-mapping";
    }));
  ui::Ref<FormField> current;
  take(ui::mount(*scene, ui::form(edit, ui::field("Optional", &City::name).reference(current))));
  take(take(navigation.managed()).navigate({1, 1}));
  paintEditors(*app, *scene, {current});
  for (auto value : {"", "old-text", "old-legacy", "old-mapping"}) {
    CAPTURE(std::string_view(value));
    take(name.set(value));
    CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.name == value);
  }
  CHECK(take(name.set("blocked")) == EditStatus::INVALID);
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.name == "old-mapping");
  (void)abandoned;
}

TEST_CASE("recreated form policy replaces required text and mapping rules but preserves the model rule") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::IMMEDIATE});
  auto name = edit.field(&City::name).validate([](const std::string &value) {
    return value != "blocked";
  });
  const auto content = take(ui::mount(*scene, ui::vbox()));
  ui::Ref<FormField> first;
  take(ui::detail::mountAt(*scene, content,
                           ui::form(edit, ui::field("Required", &City::name)
                                            .options({.required = true,
                                                      .validate =
                                                        [](std::string_view value) {
                                                          return value == "old-text"
                                                                   ? FieldValidation::invalid("Old text rule")
                                                                   : FieldValidation::valid();
                                                        }})
                                            .validate([](const std::string &value) {
                                              return value != "old-mapping";
                                            })
                                            .reference(first)),
                           ui::ControlContext{}));
  take(take(navigation.managed()).navigate({1, 1}));
  paintEditors(*app, *scene, {first});
  CHECK(take(name.set("")) == EditStatus::INVALID);
  take(scene->remove(first.node()));
  ui::Ref<FormField> current;
  take(ui::detail::mountAt(*scene, content, ui::form(edit, ui::field("Optional", &City::name).reference(current)),
                           ui::ControlContext{}));
  paintEditors(*app, *scene, {current});
  // Remount retains the inherited dirty text; the unchanged-text setter does not republish it.
  take(edit.commit().result());
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.name.empty());
  for (auto value : {"", "old-text", "old-mapping"}) {
    CAPTURE(std::string_view(value));
    take(name.set(value));
    CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.name == value);
  }
  CHECK(take(name.set("blocked")) == EditStatus::INVALID);
}

TEST_CASE("unmounted async form policy does not change typed publication before the next paint") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}, City{"B", 20, "R", 2.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::IMMEDIATE});
  auto name = edit.field(&City::name).validate([](const std::string &value) {
    return value != "blocked";
  });
  ui::Ref<FormField> current;
  take(ui::mount(*scene, ui::form(edit, ui::field("Optional", &City::name).reference(current))));
  auto managed = take(navigation.managed());
  take(managed.navigate({1, 1}));
  paintEditors(*app, *scene, {current});
  auto abandoned = ui::form(edit, ui::field("Unused async", &City::name)
                                    .options({.required = true, .validateAsync = [](std::string_view, std::stop_token) {
                                                return FieldValidation::valid();
                                              }}));
  take(managed.navigate({2, 1}));
  CHECK(take(name.set("allowed")) == EditStatus::ACCEPTED);
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().back().value.name == "allowed");
  CHECK(take(current.text().get()) == "A");
  paintEditors(*app, *scene, {current});
  CHECK(take(current.text().get()) == "allowed");
  CHECK(take(name.set("blocked")) == EditStatus::INVALID);
  (void)abandoned;
}

class ControlledValidationExecutor final : public BackgroundExecutor {
public:
  std::vector<std::shared_ptr<BackgroundJob>> jobs = std::vector<std::shared_ptr<BackgroundJob>>(0);

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
    const auto job = jobs.at(index);

    job->run(job->stopToken());
    take(job->publish());
  }
};

TEST_CASE("numeric form async validation publishes only matching replies and keeps the newer record input") {
  ControlledValidationExecutor executor;
  auto app = take(Application::create({.backgroundExecutor = &executor}));
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}, City{"B", 20, "R", 2.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});
  ui::Ref<FormField> field;
  take(ui::mount(*scene, ui::form(edit, ui::field("Orders", &City::orders)
                                          .options({.validateAsync =
                                                      [](std::string_view, std::stop_token) {
                                                        return FieldValidation::valid("Checked");
                                                      }})
                                          .reference(field))));
  auto current = take(navigation.managed());
  take(current.navigate({1, 1}));
  paintEditors(*app, *scene, {field});
  take((field.text() = "42").result());
  take(edit.commit().result());
  take(scene->afterDispatch(*app));
  REQUIRE(executor.jobs.size() == 1);
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.orders == 10);
  executor.finish(0);
  paintEditors(*app, *scene, {field});
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.orders == 42);
  take((field.text() = "43").result());
  take(edit.commit().result());
  take(scene->afterDispatch(*app));
  REQUIRE(executor.jobs.size() == 2);
  take(current.navigate({2, 1}));
  paintEditors(*app, *scene, {field});
  take((field.text() = "55").result());
  take(edit.commit().result());
  take(scene->afterDispatch(*app));
  REQUIRE(executor.jobs.size() == 3);
  executor.finish(1);
  paintEditors(*app, *scene, {field});
  CHECK(take(field.text().get()) == "55");
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.orders == 42);
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().back().value.orders == 20);
  executor.finish(2);
  paintEditors(*app, *scene, {field});
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().back().value.orders == 55);
}

TEST_CASE("saved typed draft without an editor does not format through a removed form policy") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});
  auto failFormat = std::make_shared<bool>(false);
  auto formats = std::make_shared<int>(0);
  ui::Ref<FormField> field;
  take(ui::mount(
    *scene,
    ui::form(edit,
             ui::field("Orders", &City::orders)
               .codec({.format = [failFormat, formats](const int &value) -> std::expected<std::string, TreeError> {
                         ++*formats;
                         if (*failFormat) {
                           return std::unexpected(TreeError::NO_MEMORY);
                         }
                         const auto text = std::to_string(value);
                         return std::expected<std::string, TreeError>(std::in_place, text);
                       },
                       .parse = [](std::string_view text) -> std::expected<int, TreeError> {
                         int value{};
                         const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
                         if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
                           return std::unexpected(TreeError::INVALID_ARGUMENT);
                         }
                         return value;
                       }})
               .reference(field))));
  take(take(navigation.managed()).navigate({1, 1}));
  paintEditors(*app, *scene, {field});
  take((field.text() = "12").result());
  take(scene->remove(field.node()));
  const auto previousFormats = *formats;
  *failFormat = true;
  take(edit.commit().result());
  CHECK(*formats == previousFormats);
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.orders == 12);
}

TEST_CASE("simultaneous form policies use the preferred live binding without intersecting rules") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::IMMEDIATE});
  const auto content = take(ui::mount(*scene, ui::vbox()));
  ui::Ref<FormField> first;
  ui::Ref<FormField> second;
  take(ui::detail::mountAt(*scene, content,
                           ui::form(edit, ui::field("First", &City::name)
                                            .validate([](const std::string &value) {
                                              return value != "first-only";
                                            })
                                            .reference(first)),
                           ui::ControlContext{}));
  take(ui::detail::mountAt(*scene, content,
                           ui::form(edit, ui::field("Second", &City::name)
                                            .required()
                                            .validate([](const std::string &value) {
                                              return value != "second-only";
                                            })
                                            .reference(second)),
                           ui::ControlContext{}));
  take(take(navigation.managed()).navigate({1, 1}));
  paintEditors(*app, *scene, {first, second});
  auto name = edit.field(&City::name);
  CHECK(take(name.set("")) == EditStatus::ACCEPTED);
  CHECK(take(name.set("second-only")) == EditStatus::ACCEPTED);
  CHECK(take(name.set("first-only")) == EditStatus::INVALID);
  take(scene->remove(first.node()));
  CHECK(take(name.set("")) == EditStatus::INVALID);
  CHECK(take(name.set("first-only")) == EditStatus::ACCEPTED);
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.name == "first-only");
}

TEST_CASE("custom factory receives a local typed rule without changing the shared field rule") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::IMMEDIATE});
  (void)edit.field(&City::orders).validate([](int value) {
    return value < 100;
  });
  const auto content = take(ui::mount(*scene, ui::vbox()));
  auto factoryField = std::make_shared<ui::EditField<City, int>>();
  auto codec = ui::detail::defaultFormCodec<int>();
  ui::Ref<FormField> custom;
  take(
    ui::detail::mountAt(*scene, content,
                        ui::form(edit, ui::field("Custom", &City::orders)
                                         .validate([](int value) {
                                           return value >= 0;
                                         })
                                         .editor([factoryField, codec](ui::EditField<City, int> field, bool readOnly) {
                                           *factoryField = field;
                                           return ui::lineEdit(std::move(field), codec).readOnly(readOnly);
                                         })
                                         .reference(custom)),
                        ui::ControlContext{}));
  take(take(navigation.managed()).navigate({1, 1}));
  paintEditors(*app, *scene, {custom});
  CHECK(take(factoryField->set(-1)) == EditStatus::INVALID);
  take((custom.text() = "-2").result());
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.orders == 10);
  CHECK(take(factoryField->set(200)) == EditStatus::INVALID);
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.orders == 10);
  CHECK(take(factoryField->set(12)) == EditStatus::ACCEPTED);
  take(scene->remove(custom.node()));
  ui::Ref<FormField> current;
  take(ui::detail::mountAt(*scene, content, ui::form(edit, ui::field("Unrestricted", &City::orders).reference(current)),
                           ui::ControlContext{}));
  paintEditors(*app, *scene, {current});
  CHECK(take(edit.field(&City::orders).set(-1)) == EditStatus::ACCEPTED);
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.orders == -1);
}

TEST_CASE("removing an async editor cannot approve its retained generation but a new policy can replace it") {
  ControlledValidationExecutor executor;
  auto app = take(Application::create({.backgroundExecutor = &executor}));
  auto scene = take(ManagedScene::create(*app));
  auto model = ui::model(std::array{City{"A", 10, "R", 1.0}});
  auto navigation = ui::navigation(model);
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});
  const auto content = take(ui::mount(*scene, ui::vbox()));
  ui::Ref<FormField> first;
  take(ui::detail::mountAt(*scene, content,
                           ui::form(edit, ui::field("Async", &City::orders)
                                            .options({.validateAsync =
                                                        [](std::string_view, std::stop_token) {
                                                          return FieldValidation::valid();
                                                        }})
                                            .reference(first)),
                           ui::ControlContext{}));
  take(take(navigation.managed()).navigate({1, 1}));
  paintEditors(*app, *scene, {first});
  take((first.text() = "12").result());
  take(edit.commit().result());
  take(scene->afterDispatch(*app));
  REQUIRE(executor.jobs.size() == 1);
  take(scene->remove(first.node()));
  CHECK_FALSE(edit.commit().result());
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.orders == 10);
  ui::Ref<FormField> current;
  take(ui::detail::mountAt(*scene, content, ui::form(edit, ui::field("Synchronous", &City::orders).reference(current)),
                           ui::ControlContext{}));
  paintEditors(*app, *scene, {current});
  CHECK(take(current.text().get()) == "12");
  take(edit.commit().result());
  CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.orders == 12);
}

TEST_CASE("prepaint policy reentry owns the outer candidate and rejects it before post-callback parsing") {
  for (bool customCodec : {false, true}) {
    CAPTURE(customCodec);

    struct Probe {
      bool armed{};
      bool reentered{};
      bool afterInner{};
      int supersededParses{};
      std::expected<EditStatus, TreeError> innerResult = std::unexpected(TreeError::INVALID_ARGUMENT);
      std::string observedAfterInner{"", 0};
    };

    auto probe = std::make_shared<Probe>();
    auto fieldOwner = std::make_shared<ui::EditField<City, std::string>>();
    const std::string outerText(1024, 'o');
    const std::string innerText(3072, 'i');
    auto app = take(Application::create());
    auto scene = take(ManagedScene::create(*app));
    auto model = ui::model(std::array{City{"A", 10, "R", 1.0}, City{std::string(512, 'b'), 20, "R", 2.0}});
    auto navigation = ui::navigation(model);
    auto edit = ui::edit(navigation, {.mode = EditMode::IMMEDIATE});
    *fieldOwner = edit.field(&City::name);
    ui::FormFieldConfig config;

    config.validate = [probe, weak = std::weak_ptr(fieldOwner), innerText, customCodec](std::string_view candidate) {
      if (probe->armed && !probe->reentered) {
        probe->reentered = true;
        if (auto field = weak.lock()) {
          probe->innerResult = field->set(innerText);
        }
        probe->afterInner = true;
        if (!customCodec) {
          // A validator may keep using its candidate until it returns, including after reentry.
          probe->observedAfterInner.assign(candidate);
        }
      }

      return FieldValidation::valid();
    };
    auto mapping = ui::field("Name", &City::name).options(config);
    if (customCodec) {
      mapping.codec({.format = [](const std::string &value) -> std::expected<std::string, TreeError> {
                       return std::expected<std::string, TreeError>(std::in_place, value);
                     },
                     .parse = [probe](std::string_view candidate) -> std::expected<std::string, TreeError> {
                       if (probe->afterInner) {
                         // Do not dereference a possibly invalid candidate merely to observe the stale parse.
                         ++probe->supersededParses;
                         return std::unexpected(TreeError::CALLBACK_FAILED);
                       }

                       return std::expected<std::string, TreeError>(std::in_place, candidate);
                     }});
    }
    ui::Ref<FormField> editor;
    take(ui::mount(*scene, ui::form(edit, mapping.reference(editor))));
    auto current = take(navigation.managed());
    take(current.navigate({2, 1}));
    paintEditors(*app, *scene, {editor});
    const auto targetB = take(fieldOwner->snapshot()).target;
    take(current.navigate({1, 1}));
    paintEditors(*app, *scene, {editor});
    CHECK(take(editor.text().get()) == "A");
    take(current.navigate({2, 1}));
    probe->armed = true;
    const auto result = fieldOwner->set(outerText);
    probe->armed = false;
    probe->afterInner = false;
    REQUIRE(probe->innerResult);
    CHECK(*probe->innerResult == EditStatus::ACCEPTED);
    CHECK(probe->supersededParses == 0);
    CHECK(result);
    if (result) {
      CHECK(*result == EditStatus::CONFLICT);
    }
    if (!customCodec) {
      CHECK(probe->observedAfterInner == outerText);
    }
    CHECK(ui::detail::EditAccess::sequence(model)->current->rows().front().value.name == "A");
    CHECK(ui::detail::EditAccess::sequence(model)->current->rows().back().value.name == innerText);
    CHECK(*take(fieldOwner->draftText(targetB)) == innerText);
    CHECK(take(editor.text().get()) == "A");
    paintEditors(*app, *scene, {editor});
    CHECK(take(editor.text().get()) == innerText);
  }
}

} // namespace
