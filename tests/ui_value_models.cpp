// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/model_position.hpp>
#include <portals/ui.hpp>

#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>

struct FormattedProbe {
  int value;
  inline static int calls{};

  bool operator==(const FormattedProbe &) const = default;
};

template <> struct std::formatter<FormattedProbe> : std::formatter<int> {
  template <class FormatContext> auto format(const FormattedProbe &value, FormatContext &context) const {
    ++FormattedProbe::calls;
    if (value.value < 0) {
      throw std::format_error("rejected probe");
    }

    return std::formatter<int>::format(value.value / 10, context);
  }
};

static_assert(std::formattable<FormattedProbe, char>);
static_assert(std::formattable<const FormattedProbe &, char>);
static_assert(portals::ui::detail::TextFormatArgument<portals::ui::Model<FormattedProbe> &>);

namespace no_text_import {
template <class Source> auto expression(Source source) {
  return text("ADL {}", source);
}
} // namespace no_text_import

namespace {
thread_local std::ptrdiff_t admissionAllocationsBeforeFailure = -1;
}

// This process-local seam is armed only while admitting one scalar declaration.
// Reset before throwing so cleanup and diagnostic assertions can allocate normally.
void *operator new(std::size_t size) {
  if (admissionAllocationsBeforeFailure == 0) {
    admissionAllocationsBeforeFailure = -1;
    throw std::bad_alloc();
  }
  if (admissionAllocationsBeforeFailure > 0) {
    --admissionAllocationsBeforeFailure;
  }

  auto *memory = std::malloc(size ? size : 1);
  if (!memory) {
    throw std::bad_alloc();
  }

  return memory;
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void *memory) noexcept {
  std::free(memory);
}

void operator delete[](void *memory) noexcept {
  std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
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

void operator delete(void *memory, const std::nothrow_t &) noexcept {
  ::operator delete(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  ::operator delete[](memory);
}

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value.has_value());
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Fixture {
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;

  explicit Fixture(ManagedOptions options = {}, std::size_t capacity = 8) {
    ApplicationOptions application;

    application.scheduler.data_capacity = capacity;
    app = take(Application::create(application));
    scene = take(ManagedScene::create(*app, options));
  }

  void prepare() {
    take(scene->prepare(*app, {40, 12}));
  }
};

struct ValueFrameSink final : PaintContext {
  std::vector<std::string> text;
  std::vector<RenderCommand> borrowed;
  std::string top;

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      borrowed.push_back(command);
      if (const auto *run = std::get_if<TextCommand>(&command)) {
        text.emplace_back(run->text.bytes());
      }
    }

    return {};
  }
};

ValueFrameSink paintValueFrame(Fixture &fixture, Node<Border> node, CellSize size = {40, 12}) {
  take(fixture.scene->prepare(*fixture.app, size));
  const auto bounds = take(CellRect::from({}, size));
  ValueFrameSink sink;

  take(fixture.app->tree().frame([&] {
    take(layoutTree(fixture.app->tree(), fixture.app->root(), bounds, bounds));
    take(fixture.app->tree().paint(node, sink));
    const auto buffer = take(renderFullFrame(size, sink.borrowed, fixture.scene->widthPolicy()));

    for (std::int32_t x = 0; x < size.width; ++x) {
      sink.top += take(buffer.at({x, 0})).glyph;
    }

    // Commands borrow widget text only until this enclosing tree frame returns.
    sink.borrowed.clear();
  }));

  return sink;
}

// Private admission isolates root Property receipts/capacity from caption adapter observers.
template <std::equality_comparable T> ManagedState<T> registerValue(ManagedScene &scene, const ui::Model<T> &model) {
  ui::detail::MountContext context;
  auto value = take(ui::detail::ValueModelAccess::resolve(scene, model, context));

  ui::detail::ModelAccess::publish(context);

  return value;
}

enum class Mode {
  FIRST,
  SECOND
};

struct Record {
  int count;
  bool enabled;

  bool operator==(const Record &) const = default;
};

struct NonEquality {
  int value;
};

struct Noncopyable {
  Noncopyable() = default;
  Noncopyable(const Noncopyable &) = delete;
  Noncopyable &operator=(const Noncopyable &) = delete;
};

template <class T> class EmptyProvider final : public ModelSnapshot<T> {
public:
  EmptyProvider() : ModelSnapshot<T>(1, 0) {
  }

  ModelRows<T> read(RowRange) const override {
    ++reads;

    return {};
  }

  mutable int reads{};
};

static_assert(std::same_as<decltype(ui::model(42)), ui::Model<int>>);
static_assert(std::same_as<decltype(ui::model(false)), ui::Model<bool>>);
static_assert(std::same_as<decltype(ui::model(Mode::FIRST)), ui::Model<Mode>>);
static_assert(std::same_as<decltype(ui::model(Record{1, true})), ui::Model<Record>>);
static_assert(std::same_as<decltype(ui::model("copied")), ui::Model<std::string>>);
static_assert(std::same_as<decltype(ui::model(std::array{1, 2})), ui::Model<int>>);
static_assert(
  std::same_as<decltype(ui::model(std::declval<std::shared_ptr<const ModelSnapshot<int>> &>())), ui::Model<int>>);
static_assert(sizeof(ui::Model<int>) == sizeof(ui::Model<NonEquality>));

TEST_CASE("scalar values share the public descriptor and preserve typed Property receipts") {
  Fixture fixture;
  auto number = ui::model(42);
  auto boolean = ui::model(false);
  auto mode = ui::model(Mode::FIRST);
  auto record = ui::model(Record{1, true});
  std::string original = "initial";
  auto text = ui::model(original);
  auto literal = ui::model("literal");

  original = "changed outside";
  registerValue(*fixture.scene, number);
  registerValue(*fixture.scene, boolean);
  registerValue(*fixture.scene, mode);
  registerValue(*fixture.scene, record);
  registerValue(*fixture.scene, text);
  registerValue(*fixture.scene, literal);

  CHECK(take(number.value().snapshot())->value == 42);
  CHECK_FALSE(take(boolean.value().snapshot())->value);
  CHECK(take(mode.value().snapshot())->value == Mode::FIRST);
  CHECK(take(record.value().snapshot())->value == Record{1, true});
  CHECK(take(text.value().snapshot())->value == "initial");
  CHECK(take(literal.value().snapshot())->value == "literal");
  CHECK(take(mode.value().set(Mode::SECOND)).changed);
  CHECK(take(record.value().set(Record{2, false})).changed);
  CHECK(take(text.value().set("replacement")).changed);

  const auto before = take(number.value().snapshot());
  const auto unchanged = take(number.value().set(42));

  CHECK_FALSE(unchanged.changed);
  CHECK(unchanged.snapshot == before);
  CHECK(unchanged.accepted == 0);
  CHECK(unchanged.rejected == 0);

  auto alias = number;
  auto receipt = alias.value() = 43;

  CHECK(take(receipt.result()).changed);
  CHECK(take(number.value().snapshot())->value == 43);
  CHECK(take(number.value().snapshot())->revision == 1);
  CHECK_FALSE(number.managed());

  auto other = ui::model(99);
  registerValue(*fixture.scene, other);
  alias = other;
  CHECK(take(number.value().snapshot())->value == 43);
  CHECK(take(alias.value().snapshot())->value == 99);
  take((number.value() = alias.value()).result());
  CHECK(take(number.value().snapshot())->value == 99);
}

TEST_CASE("scalar labels share source publication and ordinary managed caption delivery") {
  Fixture fixture;
  auto count = ui::model(1);
  auto enabled = ui::model(true);
  auto name = ui::model("Name");
  ui::Ref<Label> first;
  ui::Ref<Label> second;
  ui::Ref<Label> boolean;
  ui::Ref<Label> string;

  take(ui::mount(*fixture.scene, ui::column(ui::label(count).reference(first), ui::label(count).reference(second),
                                            ui::label(enabled).reference(boolean), ui::label(name).reference(string))));

  CHECK(take(first.text().get()) == "1");
  CHECK(take(second.text().get()) == "1");
  CHECK(take(boolean.text().get()) == "true");
  CHECK(take(string.text().get()) == "Name");

  const auto changed = take(count.value().set(2));

  CHECK(changed.changed);
  CHECK(changed.accepted == 2);
  CHECK(take(count.value().snapshot()) == changed.snapshot);
  fixture.prepare();
  fixture.prepare();
  CHECK(take(first.text().get()) == "2");
  CHECK(take(second.text().get()) == "2");

  take(enabled.value().set(false));
  take(name.value().set("Updated"));
  fixture.prepare();
  fixture.prepare();
  CHECK(take(boolean.text().get()) == "false");
  CHECK(take(string.text().get()) == "Updated");

  take(fixture.scene->remove(first.node()));
  take(fixture.scene->remove(second.node()));
  CHECK(take(count.value().set(3)).changed);
}

TEST_CASE("notification rejection preserves the accepted scalar commit receipt") {
  Fixture fixture({}, 1);
  auto value = ui::model(1);
  auto state = registerValue(*fixture.scene, value);

  take(fixture.scene->observe(state, [](const int &) {
  }));
  take(fixture.scene->observe(state, [](const int &) {
  }));
  fixture.prepare();

  const auto changed = take(value.value().set(2));

  CHECK(changed.changed);
  CHECK(changed.accepted == 1);
  CHECK(changed.rejected == 1);
  CHECK(changed.notification_error.has_value());
  CHECK(take(value.value().snapshot()) == changed.snapshot);
  CHECK(changed.snapshot->value == 2);
}

TEST_CASE("checked scalar errors stay checked while assignment reaches the existing collector") {
  Fixture fixture;
  auto value = ui::model(3);
  auto unopened = ui::model(4);
  ui::Model<int> invalid;

  CHECK_FALSE(unopened.value().set(5));
  take(ui::mount(*fixture.scene, ui::label(value)));
  CHECK_FALSE(invalid.value().set(1));
  CHECK(take(fixture.scene->diagnostics()).errors.empty());

  const auto assigned = value.value() = unopened.value();

  CHECK_FALSE(assigned.result());
  CHECK(take(value.value().snapshot())->value == 3);
  const auto diagnostics = take(fixture.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(std::get<TreeError>(diagnostics.errors.front().error.cause) == TreeError::CLOSED);
}

TEST_CASE("scalar values reject foreign scenes wrong threads and closed owners") {
  Fixture fixture;
  Fixture foreign;
  auto value = ui::model(3);

  take(ui::mount(*fixture.scene, ui::label(value)));
  auto rejected = ui::mount(*foreign.scene, ui::label(value));

  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::WRONG_SESSION);

  TreeError writeError = TreeError::INVALID_ARGUMENT;
  TreeError readError = TreeError::INVALID_ARGUMENT;
  std::thread worker([&] {
    const auto written = value.value().set(4);
    const auto read = value.value().snapshot();

    if (!written) {
      writeError = std::get<TreeError>(written.error().cause);
    }
    if (!read) {
      readError = std::get<TreeError>(read.error().cause);
    }
  });

  worker.join();
  CHECK(writeError == TreeError::WRONG_THREAD);
  CHECK(readError == TreeError::WRONG_THREAD);
  take(fixture.scene->close());
  const auto closed = value.value().set(4);

  REQUIRE_FALSE(closed);
  CHECK(std::get<TreeError>(closed.error().cause) == TreeError::CLOSED);
  CHECK_FALSE(value.value().snapshot());
}

TEST_CASE("unpublished scalar assignment participates in the existing facade setup collector") {
  auto unopened = ui::model(1);
  int later{};
  auto scene = ui::scene(ui::label("setup"));

  scene.configure([unopened](ManagedScene &) {
    unopened.value() = 2;
  });
  scene.configure([&later](ManagedScene &) {
    ++later;
  });
  scene.prepare();

  const auto failure = scene.checked();

  REQUIRE_FALSE(failure);
  CHECK(later == 0);
  CHECK(failure.error().setupStopped);
  CHECK(std::get<TreeError>(failure.error().cause) == TreeError::CLOSED);
  REQUIRE(failure.error().operation);
  CHECK(std::string_view(failure.error().operation) == "model.value");
}

TEST_CASE("scalar assignment preserves its receipt when diagnostic observer reentry is rejected") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto value = ui::model(1);
  auto unopened = ui::model(2);

  take(ui::mount(*scene, ui::label(value)));
  bool notified{};
  TreeError closeError = TreeError::INVALID_ARGUMENT;

  take(scene->onError([&](const ManagedDiagnostic &) {
    const auto closed = scene->close();

    notified = true;
    if (!closed) {
      closeError = std::get<TreeError>(closed.error().cause);
    }
  }));

  const auto assigned = value.value() = unopened.value();

  CHECK(notified);
  CHECK(closeError == TreeError::WOULD_BLOCK);
  CHECK_FALSE(assigned.result());
  CHECK(std::get<TreeError>(assigned.result().error().cause) == TreeError::CLOSED);
  CHECK(take(value.value().snapshot())->value == 1);

  scene.reset();
  CHECK_FALSE(scene);
  CHECK_FALSE(value.value().snapshot());
}

TEST_CASE("scalar publication allocation failure retires its registered Property before retry") {
  constexpr std::ptrdiff_t MAX_ADMISSION_ALLOCATIONS = 32;
  bool publicationFailure{};

  // Locate only the new private resolve admission's publication boundary; stop
  // as soon as rollback exists but publication could not be appended.
  for (std::ptrdiff_t budget = 0; budget < MAX_ADMISSION_ALLOCATIONS && !publicationFailure; ++budget) {
    Fixture fixture({.maxStates = 1});
    auto value = ui::model(1);

    {
      ui::detail::MountContext context;

      context.rollback.reserve(1);
      admissionAllocationsBeforeFailure = budget;

      const auto resolved = ui::detail::ValueModelAccess::resolve(*fixture.scene, value, context);

      admissionAllocationsBeforeFailure = -1;
      publicationFailure = !resolved && !context.rollback.empty() && context.publish.empty();
      if (publicationFailure) {
        CHECK(std::get<TreeError>(resolved.error().cause) == TreeError::NO_MEMORY);
      }
    }

    if (publicationFailure) {
      CHECK_FALSE(value.value().snapshot());
      registerValue(*fixture.scene, value);
      CHECK(take(value.value().snapshot())->value == 1);
      CHECK(take(value.value().set(2)).changed);
    }
  }

  REQUIRE(publicationFailure);
}

TEST_CASE("failed scalar mount detaches the attempted registration and restores state capacity") {
  Fixture fixture({.maxStates = 1});
  auto value = ui::model("initial");
  const auto rejected = ui::mount(
    *fixture.scene, ui::label(value).setup([](ManagedScene &, Node<Label> node) -> std::expected<void, ManagedError> {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }));

  REQUIRE_FALSE(rejected);
  CHECK_FALSE(value.value().snapshot());
  take(ui::mount(*fixture.scene, ui::label(value)));
  CHECK(take(value.value().snapshot())->value == "initial");
  CHECK(take(value.value().set("retry")).changed);
}

TEST_CASE("scalar declarations do not constrain unrelated provider or owned-array row types") {
  Fixture fixture;
  auto provider = std::make_shared<EmptyProvider<NonEquality>>();
  auto immobile = std::make_shared<EmptyProvider<Noncopyable>>();
  auto rows = ui::model<NonEquality>(provider);
  auto noncopyable = ui::model<Noncopyable>(immobile);
  auto array = ui::model(std::array{NonEquality{1}, NonEquality{2}});

  CHECK(provider->reads == 0);
  CHECK(immobile->reads == 0);
  take(ui::mount(*fixture.scene, ui::column(ui::virtualList(rows, &NonEquality::value),
                                            ui::virtualList(noncopyable,
                                                            [](const Noncopyable &) {
                                                              return 1;
                                                            }),
                                            ui::virtualList(array, &NonEquality::value))));
  CHECK(rows.managed());
  CHECK(noncopyable.managed());
  CHECK(array.managed());
  CHECK(provider->reads == 0);
  CHECK(immobile->reads == 0);
}

struct RetirementHook {
  std::function<void()> onDestroy;
};

struct RetiringValue {
  int value;
  std::shared_ptr<RetirementHook> hook;

  ~RetiringValue() {
    if (hook->onDestroy) {
      auto callback = std::exchange(hook->onDestroy, {});

      callback();
    }
  }

  bool operator==(const RetiringValue &other) const {
    return value == other.value;
  }
};

TEST_CASE("scalar rollback detaches declaration metadata before a value destructor destroys the owner") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto hook = std::make_shared<RetirementHook>();
  auto value = ui::model(RetiringValue{1, hook});

  {
    ui::detail::MountContext context;
    take(ui::detail::ValueModelAccess::resolve(*scene, value, context));
    hook->onDestroy = [&] {
      scene.reset();
    };
  }

  CHECK_FALSE(scene);
  CHECK_FALSE(value.value().snapshot());
}

struct Region {
  std::string name;
  int population;
  bool enabled;

  bool operator==(const Region &) const = default;
};

struct ReadOnlyRecord {
  const int code;

  bool operator==(const ReadOnlyRecord &) const = default;
};

struct BorrowedRecord {
  std::string_view view;

  bool operator==(const BorrowedRecord &) const = default;
};

template <class T, class Member>
concept FieldAvailable = requires(ui::Model<T> model, Member T::*member) { model.field(member); };

template <class Field>
concept WritableField = requires(Field field, const typename Field::Value &value) {
  field.set(value);
  field = value;
};

static_assert(WritableField<decltype(std::declval<ui::Model<Region>>().field(&Region::population))>);
static_assert(!WritableField<decltype(std::declval<ui::Model<ReadOnlyRecord>>().field(&ReadOnlyRecord::code))>);
static_assert(
  !std::is_copy_assignable_v<decltype(std::declval<ui::Model<ReadOnlyRecord>>().field(&ReadOnlyRecord::code))>);
static_assert(std::same_as<decltype(std::declval<ui::Model<Region>>().field(&Region::population).set(1)),
                           std::expected<PropertyUpdate<Region>, ManagedError>>);
static_assert(!FieldAvailable<BorrowedRecord, std::string_view>);

TEST_CASE("typed projected captions and fields share one scalar registration and preserve other fields") {
  // One root and three existing derived captions exhaust this budget exactly.
  Fixture fixture({.maxStates = 4});
  auto region = ui::model(Region{"Europe", 742, true});
  auto population = region.field(&Region::population);
  ui::Ref<Label> name;
  ui::Ref<Label> count;
  ui::Ref<Label> enabled;

  take(ui::mount(*fixture.scene, ui::column(ui::label(name, region, &Region::name), ui::label(count, population),
                                            ui::label(enabled, region, [](const Region &value) {
                                              return value.enabled;
                                            }))));

  CHECK(take(name.text().get()) == "Europe");
  CHECK(take(count.text().get()) == "742");
  CHECK(take(enabled.text().get()) == "true");

  const auto before = take(population.snapshot());
  const auto assigned = population = 743;
  const auto changed = take(assigned.result());

  CHECK(changed.changed);
  CHECK(changed.snapshot == take(region.value().snapshot()));
  CHECK(changed.snapshot->value.name == "Europe");
  CHECK(changed.snapshot->value.population == 743);
  CHECK(changed.snapshot->value.enabled);
  CHECK(before->value == 742);
  CHECK(before->revision == 0);
  CHECK(take(population.snapshot())->revision == changed.snapshot->revision);
  CHECK_FALSE(population.result());

  fixture.prepare();
  CHECK(take(count.text().get()) == "743");
  CHECK(take(name.text().get()) == "Europe");

  const auto equal = take(population.set(743));

  CHECK_FALSE(equal.changed);
  CHECK(equal.accepted == 0);
  CHECK(equal.snapshot == changed.snapshot);
  take(region.value().set(Region{"Renamed", 800, false}));
  take(population.set(801));
  fixture.prepare();
  CHECK(take(name.text().get()) == "Renamed");
  CHECK(take(count.text().get()) == "801");
  CHECK(take(enabled.text().get()) == "false");
  CHECK(before->value == 742);
}

TEST_CASE("field writes retain exact root notification rejection receipts") {
  Fixture fixture({}, 1);
  auto region = ui::model(Region{"Europe", 742, true});
  auto state = registerValue(*fixture.scene, region);

  take(fixture.scene->observe(state, [](const Region &) {
  }));
  take(fixture.scene->observe(state, [](const Region &) {
  }));
  fixture.prepare();

  const auto changed = take(region.field(&Region::population).set(743));

  CHECK(changed.changed);
  CHECK(changed.accepted == 1);
  CHECK(changed.rejected == 1);
  CHECK(changed.notification_error.has_value());
  CHECK(changed.snapshot == take(region.value().snapshot()));
  CHECK(changed.snapshot->value.population == 743);
}

TEST_CASE("field descriptors own their source and snapshots survive root replacement and owner destruction") {
  Fixture fixture;
  auto field = [] {
    auto temporary = ui::model(Region{"Owned", 1, true});

    return temporary.field(&Region::name);
  }();
  ui::Ref<Label> label;

  take(ui::mount(*fixture.scene, ui::label(label, field)));
  const auto saved = take(field.snapshot());
  std::string replacement = "Changed";

  take(field.set(replacement));
  replacement = "outside";
  fixture.prepare();
  CHECK(take(label.text().get()) == "Changed");
  CHECK(saved->value == "Owned");
  CHECK(saved->revision == 0);
  fixture.scene.reset();
  CHECK(saved->value == "Owned");
  CHECK_FALSE(field.snapshot());
}

TEST_CASE("field checked failures and value-copy assignment follow the existing collector") {
  Fixture fixture;
  auto target = ui::model(Region{"Target", 1, true});
  auto source = ui::model(Region{"Source", 2, false});
  auto unopened = ui::model(Region{"Cold", 3, true});
  auto invalidMember = static_cast<int Region::*>(nullptr);

  take(ui::mount(*fixture.scene, ui::column(ui::label(target, &Region::name), ui::label(source, &Region::name))));
  const auto copied = target.field(&Region::population) = source.field(&Region::population);

  CHECK((take(copied.result()).snapshot->value == Region{"Target", 2, true}));
  take(source.field(&Region::population).set(5));
  CHECK(take(target.field(&Region::population).snapshot())->value == 2);
  CHECK_FALSE(unopened.field(&Region::population).set(6));
  const auto nullField = target.field(invalidMember).snapshot();

  REQUIRE_FALSE(nullField);
  CHECK(std::get<TreeError>(nullField.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());

  const auto failed = target.field(&Region::population) = unopened.field(&Region::population);

  CHECK_FALSE(failed.result());
  CHECK(take(target.field(&Region::population).snapshot())->value == 2);
  const auto errors = take(fixture.scene->diagnostics()).errors;

  REQUIRE(errors.size() == 1);
  CHECK(std::get<TreeError>(errors.front().error.cause) == TreeError::CLOSED);
  CHECK(std::string_view(errors.front().error.operation) == "model.value");

  auto rows = ui::model(std::array{Region{"Row", 1, true}});
  const auto collectionField = rows.field(&Region::population).set(9);

  REQUIRE_FALSE(collectionField);
  CHECK(std::get<TreeError>(collectionField.error().cause) == TreeError::INVALID_ARGUMENT);
}

TEST_CASE("scalar field observations reject wrong threads foreign scenes and closed owners") {
  Fixture fixture;
  Fixture foreign;
  auto region = ui::model(Region{"Europe", 742, true});
  auto field = region.field(&Region::population);

  take(ui::mount(*fixture.scene, ui::label(field)));
  const auto rejected = ui::mount(*foreign.scene, ui::label(region, &Region::name));

  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::WRONG_SESSION);

  TreeError readError = TreeError::INVALID_ARGUMENT;
  TreeError writeError = TreeError::INVALID_ARGUMENT;
  std::thread worker([&] {
    const auto read = field.snapshot();
    const auto write = field.set(743);

    if (!read) {
      readError = std::get<TreeError>(read.error().cause);
    }
    if (!write) {
      writeError = std::get<TreeError>(write.error().cause);
    }
  });

  worker.join();
  CHECK(readError == TreeError::WRONG_THREAD);
  CHECK(writeError == TreeError::WRONG_THREAD);
  take(fixture.scene->close());
  const auto closed = field.set(743);

  REQUIRE_FALSE(closed);
  CHECK(std::get<TreeError>(closed.error().cause) == TreeError::CLOSED);
  CHECK_FALSE(field.snapshot());
}

TEST_CASE("projected captions own callables and checked projection errors retain the last valid caption") {
  Fixture fixture;
  auto region = ui::model(Region{"Europe", 742, true});
  ui::Ref<Label> caption;
  auto recipe = [&] {
    std::string prefix = "Population ";

    return ui::label(caption, region, [prefix](const Region &value) -> std::expected<std::string, TreeError> {
      if (!value.enabled) {
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }

      return prefix + std::to_string(value.population);
    });
  }();

  take(ui::mount(*fixture.scene, std::move(recipe)));
  CHECK(take(caption.text().get()) == "Population 742");
  take(region.field(&Region::enabled).set(false));
  const auto failed = fixture.scene->prepare(*fixture.app, {40, 12});

  REQUIRE_FALSE(failed);
  CHECK(failed.error() == TreeError::INVALID_GEOMETRY);
  CHECK(take(caption.text().get()) == "Population 742");
  CHECK_FALSE(take(region.value().snapshot())->value.enabled);
  const auto errors = take(fixture.scene->diagnostics()).errors;

  REQUIRE(errors.size() == 1);
  CHECK(std::get<TreeError>(errors.front().error.cause) == TreeError::INVALID_GEOMETRY);
  CHECK(std::string_view(errors.front().error.operation) == "derive.compute");
  take(region.value().set(Region{"Europe", 743, true}));
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Population 743");
}

TEST_CASE("projection setup failures release scalar and caption state budgets for a clean retry") {
  Fixture fixture({.maxStates = 2});
  auto region = ui::model(Region{"Europe", 742, true});
  const auto rejected = ui::mount(*fixture.scene, ui::label(region, [](const Region &) -> int {
    throw std::runtime_error("projection failed");
  }));

  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::CALLBACK_FAILED);
  CHECK_FALSE(region.value().snapshot());

  const auto setupRejected =
    ui::mount(*fixture.scene, ui::label(region, &Region::population)
                                .setup([](ManagedScene &, Node<Label> node) -> std::expected<void, ManagedError> {
                                  return std::unexpected(
                                    ManagedError{ManagedPhase::BIND, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
                                }));

  REQUIRE_FALSE(setupRejected);
  CHECK_FALSE(region.value().snapshot());
  take(ui::mount(*fixture.scene, ui::label(region, &Region::population)));
  CHECK(take(region.field(&Region::population).snapshot())->value == 742);
}

TEST_CASE("null member projection is a typed setup rejection without publishing the root") {
  Fixture fixture({.maxStates = 2});
  auto region = ui::model(Region{"Europe", 742, true});
  auto member = static_cast<int Region::*>(nullptr);
  const auto rejected = ui::mount(*fixture.scene, ui::label(region, member));

  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK_FALSE(region.value().snapshot());
  take(ui::mount(*fixture.scene, ui::label(region.field(&Region::population))));
  CHECK(take(region.field(&Region::population).snapshot())->value == 742);
}

TEST_CASE("projection allocation and callback exceptions preserve the last valid runtime caption") {
  for (bool allocation : {false, true}) {
    Fixture fixture;
    auto region = ui::model(Region{"Europe", 742, true});
    ui::Ref<Label> caption;

    take(ui::mount(*fixture.scene, ui::label(caption, region, [allocation](const Region &value) {
      if (!value.enabled) {
        if (allocation) {
          throw std::bad_alloc();
        }

        throw std::runtime_error("runtime projection");
      }

      return value.population;
    })));
    take(region.field(&Region::enabled).set(false));
    const auto failed = fixture.scene->prepare(*fixture.app, {40, 12});

    REQUIRE_FALSE(failed);
    CHECK(failed.error() == (allocation ? TreeError::NO_MEMORY : TreeError::CALLBACK_FAILED));
    CHECK(take(caption.text().get()) == "742");
    CHECK_FALSE(take(region.value().snapshot())->value.enabled);
  }
}

TEST_CASE("unpublished field assignment uses the existing facade setup collector") {
  auto unopened = ui::model(Region{"Cold", 3, true});
  int later{};
  auto scene = ui::scene(ui::label("setup"));

  scene.configure([unopened](ManagedScene &) {
    unopened.field(&Region::population) = 4;
  });
  scene.configure([&later](ManagedScene &) {
    ++later;
  });
  scene.prepare();
  const auto failure = scene.checked();

  REQUIRE_FALSE(failure);
  CHECK(later == 0);
  CHECK(failure.error().setupStopped);
  CHECK(std::get<TreeError>(failure.error().cause) == TreeError::CLOSED);
  CHECK(std::string_view(failure.error().operation) == "model.value");
}

TEST_CASE("read-only fields and GraphemeText are scalar values with ordinary projected caption formatting") {
  Fixture fixture;
  auto record = ui::model(ReadOnlyRecord{7});
  auto text = ui::model(take(GraphemeText::from("Owned text")));
  ui::Ref<Label> code;
  ui::Ref<Label> caption;

  take(ui::mount(*fixture.scene,
                 ui::column(ui::label(code, record.field(&ReadOnlyRecord::code)), ui::label(caption, text))));
  CHECK(take(code.text().get()) == "7");
  CHECK(take(record.field(&ReadOnlyRecord::code).snapshot())->value == 7);
  CHECK(take(caption.text().get()) == "Owned text");
  take(text.value().set(take(GraphemeText::from("Updated text"))));
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Updated text");
}

struct ThrowingField {
  int value;
  inline static bool rejectCopy{};
  inline static bool rejectAssignment{};

  explicit ThrowingField(int initial) : value(initial) {
  }

  ThrowingField(const ThrowingField &other) : value(other.value) {
    if (rejectCopy) {
      throw std::runtime_error("field copy");
    }
  }

  ThrowingField &operator=(const ThrowingField &other) {
    if (rejectAssignment) {
      throw std::runtime_error("field assignment");
    }

    value = other.value;

    return *this;
  }

  bool operator==(const ThrowingField &) const = default;
};

struct FallibleRecord {
  ThrowingField field;
  int preserved;

  bool operator==(const FallibleRecord &) const = default;
};

TEST_CASE("field copy allocation and assignment failures are checked before a root write") {
  Fixture fixture;
  auto record = ui::model(FallibleRecord{ThrowingField{1}, 9});
  auto field = record.field(&FallibleRecord::field);

  take(ui::mount(*fixture.scene, ui::label(record, &FallibleRecord::preserved)));
  const auto before = take(record.value().snapshot());
  const ThrowingField replacement{2};

  ThrowingField::rejectCopy = true;
  const auto copyFailure = field.snapshot();
  const auto rootCopyFailure = field.set(replacement);

  ThrowingField::rejectCopy = false;
  REQUIRE_FALSE(copyFailure);
  CHECK(std::get<TreeError>(copyFailure.error().cause) == TreeError::CALLBACK_FAILED);
  REQUIRE_FALSE(rootCopyFailure);
  CHECK(std::get<TreeError>(rootCopyFailure.error().cause) == TreeError::CALLBACK_FAILED);

  ThrowingField::rejectAssignment = true;
  const auto assignmentFailure = field.set(replacement);

  ThrowingField::rejectAssignment = false;
  REQUIRE_FALSE(assignmentFailure);
  CHECK(std::get<TreeError>(assignmentFailure.error().cause) == TreeError::CALLBACK_FAILED);

  admissionAllocationsBeforeFailure = 0;
  const auto allocationFailure = field.snapshot();

  admissionAllocationsBeforeFailure = -1;
  REQUIRE_FALSE(allocationFailure);
  CHECK(std::get<TreeError>(allocationFailure.error().cause) == TreeError::NO_MEMORY);
  CHECK(take(record.value().snapshot()) == before);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
  const auto changed = take(field.set(replacement));

  CHECK(changed.snapshot->value.field.value == 2);
  CHECK(changed.snapshot->value.preserved == 9);
}

TEST_CASE("borrowed projection results become owning captions and initial lifecycle reentry stays guarded") {
  Fixture fixture;
  auto region = ui::model(Region{"Europe", 742, true});
  ui::Ref<Label> caption;
  TreeError closeError = TreeError::INVALID_ARGUMENT;

  take(ui::mount(*fixture.scene, ui::label(caption, region, [&](const Region &value) -> std::string_view {
    const auto closed = fixture.scene->close();

    if (!closed) {
      closeError = std::get<TreeError>(closed.error().cause);
    }

    return value.name;
  })));
  CHECK(closeError == TreeError::WOULD_BLOCK);
  CHECK(take(caption.text().get()) == "Europe");
  take(region.field(&Region::name).set("Updated"));
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Updated");
  CHECK(closeError == TreeError::WOULD_BLOCK);
  take(fixture.scene->remove(caption.node()));
  CHECK(take(region.field(&Region::name).set("After removal")).changed);
  fixture.prepare();
}

TEST_CASE("scalar caption replacement keeps the field projection and each root's accepted value") {
  Fixture fixture;
  auto europe = ui::model(Region{"Europe", 742, true});
  auto asia = ui::model(Region{"Asia", 4770, true});
  ui::Ref<Label> caption;
  ui::Ref<Label> mirror;

  take(ui::mount(*fixture.scene, ui::column(ui::label(caption, europe.field(&Region::name)),
                                            ui::label(mirror, europe, &Region::population))));
  take(caption.setModel(asia));
  CHECK(take(caption.text().get()) == "Asia");
  take(europe.field(&Region::name).set("Updated Europe"));
  take(europe.field(&Region::population).set(743));
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Asia");
  CHECK(take(mirror.text().get()) == "743");
  take(asia.field(&Region::name).set("Updated Asia"));
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Updated Asia");
  CHECK(caption.model(europe));
  CHECK(take(caption.text().get()) == "Updated Europe");
  take(caption.setModel(asia));
  CHECK(take(caption.text().get()) == "Updated Asia");
}

TEST_CASE("scalar replacement pins the original lambda and same declaration needs no admission") {
  Fixture fixture;
  auto first = ui::model(Region{"First", 10, true});
  auto second = ui::model(Region{"Second", 20, true});
  int calls{};
  ui::Ref<Label> caption;

  take(ui::mount(*fixture.scene,
                 ui::label(caption, first, [prefix = std::string("Population: "), &calls](const Region &r) {
                   ++calls;

                   return prefix + std::to_string(r.population);
                 })));
  CHECK(calls == 1);

  admissionAllocationsBeforeFailure = 0;
  const auto same = caption.setModel(first);
  admissionAllocationsBeforeFailure = -1;

  REQUIRE(same);
  CHECK(calls == 1);
  take(caption.setModel(second));
  CHECK(take(caption.text().get()) == "Population: 20");
  CHECK(calls == 2);
  take(first.field(&Region::population).set(11));
  fixture.prepare();
  CHECK(calls == 2);
  take(second.field(&Region::population).set(21));
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Population: 21");
  CHECK(calls == 3);
}

TEST_CASE("plain scalar labels adopt a default formatter and reject incompatible capabilities") {
  Fixture fixture;
  ui::Ref<Label> caption;
  auto number = ui::model(42);
  auto record = ui::model(Region{"Record", 1, true});
  auto collection = ui::model(std::array{42});

  take(ui::mount(*fixture.scene, ui::label(caption, "Plain")));
  const auto unprojected = caption.setModel(record);

  REQUIRE_FALSE(unprojected);
  CHECK(std::get<TreeError>(unprojected.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK(take(caption.text().get()) == "Plain");
  CHECK_FALSE(caption.setModel(collection));
  take(caption.setModel(number));
  CHECK(take(caption.text().get()) == "42");
  const auto wrongType = caption.setModel(ui::model("String"));

  REQUIRE_FALSE(wrongType);
  CHECK(std::get<TreeError>(wrongType.error().cause) == TreeError::INVALID_ARGUMENT);
  take(number.value().set(43));
  fixture.prepare();
  CHECK(take(caption.text().get()) == "43");
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
  CHECK_FALSE(caption.model(record));
  const auto errors = take(fixture.scene->diagnostics()).errors;

  REQUIRE(errors.size() == 1);
  CHECK(std::string_view(errors.front().error.operation) == "model");
}

TEST_CASE("rejected scalar candidate preserves the old binding and releases attempted state admission") {
  Fixture fixture({.maxStates = 4});
  auto first = ui::model(Region{"First", 10, true});
  auto rejected = ui::model(Region{"Rejected", 20, false});
  auto accepted = ui::model(Region{"Accepted", 30, true});
  ui::Ref<Label> caption;

  take(ui::mount(*fixture.scene, ui::label(caption, first, [](const Region &r) -> std::expected<int, TreeError> {
    return r.enabled ? std::expected<int, TreeError>(r.population) : std::unexpected(TreeError::INVALID_GEOMETRY);
  })));
  const auto failed = caption.setModel(rejected);

  REQUIRE_FALSE(failed);
  CHECK(std::get<TreeError>(failed.error().cause) == TreeError::INVALID_GEOMETRY);
  CHECK_FALSE(rejected.value().snapshot());
  CHECK(take(caption.text().get()) == "10");
  take(first.field(&Region::population).set(11));
  fixture.prepare();
  CHECK(take(caption.text().get()) == "11");
  take(caption.setModel(accepted));
  CHECK(take(caption.text().get()) == "30");
  // Three retained scalar roots/output slots: a fresh state fits only if the replaced output retired.
  take(fixture.scene->state(99));
}

TEST_CASE("scalar source replacement allocation rejection leaves its working source installed") {
  bool reachedCommit{};
  int rejectedAttempts{};

  for (std::ptrdiff_t allowance = 0; allowance != 96 && !reachedCommit; ++allowance) {
    Fixture fixture;
    auto first = ui::model(Region{"First", 10, true});
    auto second = ui::model(Region{"Second", 20, true});
    ui::Ref<Label> caption;

    take(ui::mount(*fixture.scene, ui::label(caption, first, &Region::name)));

    admissionAllocationsBeforeFailure = allowance;
    const auto candidate = caption.setModel(second);
    admissionAllocationsBeforeFailure = -1;

    if (candidate) {
      reachedCommit = true;
      CHECK(take(caption.text().get()) == "Second");
    } else {
      ++rejectedAttempts;
      CHECK(std::get<TreeError>(candidate.error().cause) == TreeError::NO_MEMORY);
      CHECK(take(caption.text().get()) == "First");
      CHECK_FALSE(second.value().snapshot());
      take(first.field(&Region::name).set("Still working"));
      fixture.prepare();
      CHECK(take(caption.text().get()) == "Still working");
      CHECK(take(fixture.scene->diagnostics()).errors.empty());
    }
  }

  CHECK(reachedCommit);
  CHECK(rejectedAttempts > 0);
}

#if defined(_MSC_VER) && defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL == 2
TEST_CASE("live text admission catches the MSVC checked STL pattern and temporary roots proxy failures") {
  Fixture fixture;
  auto original = ui::model("Previous");
  auto replacement = ui::model(7);
  const auto expression = portals::text("Value {}", replacement);
  ui::Ref<Label> caption;

  take(ui::mount(*fixture.scene, ui::label(caption, original)));

  // These allowances isolate the supported MSVC checked-STL construction path, not a portable count:
  // make_shared's block, then the empty pattern's proxy, then the local roots vector's proxy.
  constexpr std::ptrdiff_t PATTERN_PROXY_ALLOWANCE = 1;
  constexpr std::ptrdiff_t ROOTS_PROXY_ALLOWANCE = 2;

  for (const auto allowance : {PATTERN_PROXY_ALLOWANCE, ROOTS_PROXY_ALLOWANCE}) {
    ui::detail::MountContext context;
    auto graph = std::make_shared<ui::detail::Access::ValueCaptionGraph>();
    const std::string pattern("", 0);
    const std::tuple<> arguments;
    bool allocationRejected{};

    graph->scene = fixture.scene.get();
    graph->lifetime = ui::detail::Access::lifetime(*fixture.scene);

    admissionAllocationsBeforeFailure = allowance;
    try {
      // An empty private argument pack reaches the actual roots constructor without input allocations.
      // The public text() API still requires at least one explicit live dependency.
      const auto candidate =
        ui::detail::ValueTextAccess::create(*fixture.scene, caption.node(), context, graph, pattern, arguments);

      (void)candidate;
    } catch (const std::bad_alloc &) {
      allocationRejected = true;
    }
    admissionAllocationsBeforeFailure = -1;

    REQUIRE(allocationRejected);
    CHECK(take(caption.text().get()) == "Previous");
    CHECK_FALSE(replacement.value().snapshot());
    CHECK(take(fixture.scene->diagnostics()).errors.empty());
  }

  take(caption.setModel(expression));
  CHECK(take(caption.text().get()) == "Value 7");
  take(replacement.value().set(8));
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Value 8");
}
#endif

TEST_CASE("scalar replacement rejects foreign threads scenes and retired recipients") {
  Fixture fixture;
  Fixture foreign;
  auto local = ui::model(1);
  auto other = ui::model(2);
  ui::Ref<Label> caption;

  take(ui::mount(*fixture.scene, ui::label(caption, local)));
  take(ui::mount(*foreign.scene, ui::label(other)));
  const auto rejected = caption.setModel(other);

  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::WRONG_SESSION);

  TreeError threadError = TreeError::INVALID_ARGUMENT;
  std::thread worker([&] {
    const auto failed = caption.setModel(local);
    if (!failed) {
      threadError = std::get<TreeError>(failed.error().cause);
    }
  });

  worker.join();
  CHECK(threadError == TreeError::WRONG_THREAD);
  take(fixture.scene->remove(caption.node()));
  CHECK_FALSE(caption.setModel(local));
}

TEST_CASE("live text checks underlying values and owns static arguments with qualified and ADL calls") {
  Fixture fixture;
  auto region = ui::model(Region{"Europe", 742, true});
  auto count = ui::model(7);
  ui::Ref<Label> field;
  ui::Ref<Label> whole;
  ui::Ref<Label> property;
  ui::Ref<Label> literal;
  ui::Ref<Label> boundedArray;
  auto expression = [&] {
    std::string suffix = "million";

    return portals::text("Population: {:04} {}", region.field(&Region::population), std::string_view(suffix));
  }();
  const auto arrayExpression = [&] {
    const char suffix[]{'X', 'Y'};

    return portals::text("{} {}", count, suffix);
  }();

  take(ui::mount(*fixture.scene, ui::column(ui::label(field, expression), ui::label(whole, text("Value {}", count)),
                                            ui::label(property, portals::text("Property {}", count.value())),
                                            ui::label(literal, portals::text("{} {}", count, "owned literal")),
                                            ui::label(boundedArray, arrayExpression))));
  CHECK(take(field.text().get()) == "Population: 0742 million");
  CHECK(take(whole.text().get()) == "Value 7");
  CHECK(take(property.text().get()) == "Property 7");
  CHECK(take(literal.text().get()) == "7 owned literal");
  CHECK(take(boundedArray.text().get()) == "7 XY");
  take(region.field(&Region::population).set(743));
  take(count.value().set(8));
  fixture.prepare();
  CHECK(take(field.text().get()) == "Population: 0743 million");
  CHECK(take(whole.text().get()) == "Value 8");
  CHECK(take(property.text().get()) == "Property 8");
  CHECK(take(literal.text().get()) == "8 owned literal");
  CHECK(take(portals::text("{} {}", "eager", 42).resolve(WidthPolicy{})).bytes() == "eager 42");
  CHECK(take(portals::text("{}").resolve(WidthPolicy{})).bytes() == "{}");
  static_assert(std::same_as<decltype(no_text_import::expression(count)), ui::ValueText>);
}

TEST_CASE("live text deduplicates shared roots at exact per-receiver observation admission") {
  Fixture fixture({.maxStates = 3, .maxObservers = 1, .maxObserversPerNode = 1});
  auto region = ui::model(Region{"Europe", 742, true});
  auto other = ui::model(1);
  ui::Ref<Label> caption;

  take(ui::mount(*fixture.scene, ui::label(caption, portals::text("{}: {}", region.field(&Region::name),
                                                                  region.field(&Region::population)))));
  CHECK(take(caption.text().get()) == "Europe: 742");
  take(region.value().set(Region{"Updated", 743, true}));
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Updated: 743");
  const auto failed = caption.setModel(portals::text("{} {}", other, region.field(&Region::population)));

  REQUIRE_FALSE(failed);
  CHECK(std::get<TreeError>(failed.error().cause) == TreeError::NODE_LIMIT);
  CHECK_FALSE(other.value().snapshot());
  CHECK(take(caption.text().get()) == "Updated: 743");
  take(region.field(&Region::population).set(744));
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Updated: 744");
}

TEST_CASE("computed text replacement replaces the complete expression and never redirects one field") {
  Fixture fixture;
  auto europe = ui::model(Region{"Europe", 742, true});
  auto asia = ui::model(Region{"Asia", 4770, true});
  const auto first = portals::text("{}: {}", europe.field(&Region::name), europe.field(&Region::population));
  const auto second = portals::text("Population: {}", asia.field(&Region::population));
  ui::Ref<Label> caption;

  take(ui::mount(*fixture.scene, ui::label(caption, first)));
  const auto scalar = caption.setModel(asia);

  REQUIRE_FALSE(scalar);
  CHECK(std::get<TreeError>(scalar.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK_FALSE(asia.value().snapshot());
  CHECK(take(caption.text().get()) == "Europe: 742");

  admissionAllocationsBeforeFailure = 0;
  const auto same = caption.setModel(first);
  admissionAllocationsBeforeFailure = -1;

  REQUIRE(same);
  CHECK(caption.model(second));
  CHECK(take(caption.text().get()) == "Population: 4770");
  take(europe.field(&Region::population).set(743));
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Population: 4770");
  take(caption.setModel(first));
  CHECK(take(caption.text().get()) == "Europe: 743");
}

TEST_CASE("replacing caption graphs releases bounded observations and derived output slots") {
  for (bool formatted : {false, true}) {
    Fixture fixture({.maxStates = 4, .maxObservers = 2, .maxObserversPerNode = 2});
    auto first = ui::model(1);
    auto second = ui::model(2);
    ui::Ref<Label> caption;

    if (formatted) {
      take(ui::mount(*fixture.scene, ui::label(caption, portals::text("{}", first))));
    } else {
      take(ui::mount(*fixture.scene, ui::label(caption, first)));
    }

    for (int iteration = 0; iteration != 10; ++iteration) {
      const auto source = iteration % 2 ? first : second;

      if (formatted) {
        take(caption.setModel(portals::text("{}", source)));
      } else {
        take(caption.setModel(source));
      }

      CHECK(take(caption.text().get()) == (iteration % 2 ? "1" : "2"));
    }

    take(fixture.scene->state(3));
  }
}

TEST_CASE("live formatter failures retain the previous valid caption and equal output stays silent") {
  Fixture fixture;
  auto source = ui::model(FormattedProbe{10});
  ui::Ref<Label> caption;

  FormattedProbe::calls = 0;
  take(ui::mount(*fixture.scene, ui::label(caption, portals::text("Value {}", source))));
  CHECK(FormattedProbe::calls == 1);
  const auto slot = take(ui::detail::Access::valueCaptionSlot(*fixture.scene, caption.node()));
  const auto graph = std::static_pointer_cast<ui::detail::Access::ValueCaptionGraph>(slot.source->graph);
  const auto output = std::static_pointer_cast<Property<std::string>>(graph->output);
  const auto initialOutput = take(output->snapshot());

  fixture.prepare();
  fixture.prepare();
  CHECK(FormattedProbe::calls == 1);
  CHECK_FALSE(take(source.value().set(FormattedProbe{10})).changed);
  fixture.prepare();
  CHECK(FormattedProbe::calls == 1);
  take(source.value().set(FormattedProbe{11}));
  fixture.prepare();
  CHECK(FormattedProbe::calls == 2);
  CHECK(take(caption.text().get()) == "Value 1");
  CHECK(take(output->snapshot()) == initialOutput);

  take(source.value().set(FormattedProbe{-1}));
  const auto failed = fixture.scene->prepare(*fixture.app, {40, 12});

  REQUIRE_FALSE(failed);
  CHECK(failed.error() == TreeError::CALLBACK_FAILED);
  CHECK(take(caption.text().get()) == "Value 1");
  take(source.value().set(FormattedProbe{20}));
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Value 2");
  take(fixture.app->tree().setVisible(caption.node(), false));
  fixture.prepare();
  const auto before = FormattedProbe::calls;

  fixture.prepare();
  CHECK(FormattedProbe::calls == before);
  take(source.value().set(FormattedProbe{30}));
  fixture.prepare();
  CHECK(FormattedProbe::calls == before + 1);
  CHECK(take(caption.text().get()) == "Value 3");
  fixture.prepare();
  CHECK(FormattedProbe::calls == before + 1);
  take(fixture.scene->remove(caption.node()));
  take(source.value().set(FormattedProbe{40}));
  fixture.prepare();
  CHECK(FormattedProbe::calls == before + 1);
}

TEST_CASE("live text rejects initial and runtime invalid UTF8 without replacing the last valid caption") {
  Fixture fixture;
  auto source = ui::model("Valid");
  auto invalid = ui::model(std::string(1, '\xff'));
  ui::Ref<Label> caption;

  take(ui::mount(*fixture.scene, ui::label(caption, portals::text("{}", source))));
  const auto rejected = caption.setModel(portals::text("{}", invalid));

  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK_FALSE(invalid.value().snapshot());
  CHECK(take(caption.text().get()) == "Valid");
  take(source.value().set(std::string(1, '\xff')));
  const auto failed = fixture.scene->prepare(*fixture.app, {40, 12});

  REQUIRE_FALSE(failed);
  CHECK(failed.error() == TreeError::INVALID_ARGUMENT);
  CHECK(take(caption.text().get()) == "Valid");
}

TEST_CASE("live format candidate rejects collection capabilities and rolls back partial scalar admission") {
  Fixture fixture({.maxStates = 4});
  auto original = ui::model(1);
  auto cold = ui::model(2);
  auto rows = ui::model(std::array{3, 4});
  ui::Ref<Label> caption;

  take(ui::mount(*fixture.scene, ui::label(caption, portals::text("{}", original))));
  const auto rejected = caption.setModel(portals::text("{} {}", cold, rows));

  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK_FALSE(cold.value().snapshot());
  CHECK(take(caption.text().get()) == "1");
  take(caption.setModel(portals::text("{}", cold)));
  CHECK(take(caption.text().get()) == "2");
}

struct RetiringCaption {
  std::unique_ptr<ManagedScene> *scene{};
  bool armed{};

  ~RetiringCaption() {
    if (armed) {
      scene->reset();
    }
  }
};

TEST_CASE("caption retirement permits scene shutdown after replacement removal and external removal") {
  for (int operation : {0, 1, 2, 3, 4}) {
    Fixture fixture;
    auto source = ui::model(1);
    auto replacement = ui::model(2);
    auto retiring = std::make_shared<RetiringCaption>();
    ui::Ref<Label> caption;

    retiring->scene = &fixture.scene;
    take(ui::mount(*fixture.scene, ui::label(caption, source, [retiring](int value) {
      return value;
    })));
    retiring->armed = true;
    retiring.reset();

    if (operation == 0) {
      take(caption.setModel(portals::text("{}", replacement)));
    } else if (operation == 1) {
      take(fixture.scene->remove(caption.node()));
    } else if (operation == 2) {
      take(fixture.app->tree().remove(caption.node()));
      fixture.prepare();
    } else if (operation == 3) {
      take(fixture.scene->close());
    } else {
      fixture.scene->stop(*fixture.app);
    }

    CHECK_FALSE(fixture.scene);
    CHECK_FALSE(caption.text().get());
    CHECK_FALSE(source.value().snapshot());
  }
}

TEST_CASE("collected caption replacement latches setup failure before later configuration") {
  auto source = ui::model(1);
  ui::Ref<Label> caption;
  int later{};
  auto scene = ui::scene(ui::label(caption, source));

  scene.configure([caption](ManagedScene &) {
    caption.model(ui::model("Wrong root type"));
  });
  scene.configure([&later](ManagedScene &) {
    ++later;
  });
  scene.prepare();
  const auto failure = scene.checked();

  REQUIRE_FALSE(failure);
  CHECK(later == 0);
  CHECK(failure.error().setupStopped);
  CHECK(std::string_view(failure.error().operation) == "model");
}

static_assert(std::same_as<decltype(ui::model(std::declval<ManagedState<int>>())), ui::Model<int>>);
static_assert(std::same_as<decltype(ui::model(std::declval<ManagedSource<int>>())), ui::Model<int>>);
static_assert(std::same_as<decltype(std::declval<ui::Navigation<int>>().current()), ui::Model<NavigationPosition>>);

struct ExistingScalar {
  explicit ExistingScalar(int value) : count(value) {
  }

  ExistingScalar(const ExistingScalar &other) : count(other.count) {
    ++copies;
  }

  ExistingScalar &operator=(const ExistingScalar &) = default;
  bool operator==(const ExistingScalar &) const = default;

  int count;
  inline static int copies{};
};

TEST_CASE("existing scalar aliases preserve the Property and need no initial value copy or state slot") {
  Fixture fixture({.maxStates = 1});
  auto state = take(fixture.scene->state(ExistingScalar(7)));
  const auto before = take(state.snapshot());
  ExistingScalar::copies = 0;
  auto first = ui::model(state);
  auto second = ui::model(state);

  CHECK(ExistingScalar::copies == 0);
  CHECK_FALSE(first.value().snapshot());
  registerValue(*fixture.scene, first);
  registerValue(*fixture.scene, second);
  CHECK(ExistingScalar::copies == 0);
  CHECK(take(first.value().snapshot()) == before);
  CHECK(take(second.value().snapshot()) == before);
  CHECK(take(first.field(&ExistingScalar::count).set(8)).changed);
  CHECK(take(state.snapshot())->revision == 1);
  CHECK(take(second.value().snapshot()) == take(state.snapshot()));
  CHECK_FALSE(fixture.scene->state(1));
  take(fixture.scene->close());
  const auto closed = first.value().snapshot();

  REQUIRE_FALSE(closed);
  CHECK(std::get<TreeError>(closed.error().cause) == TreeError::CLOSED);
}

TEST_CASE("read-only aliases share snapshots and reject whole and field writes through checked and collected paths") {
  Fixture fixture({.maxStates = 2});
  auto input = take(fixture.scene->state(1));
  auto source = take(fixture.scene->derive(input, [](const int &value) {
    return Record{value, true};
  }));
  auto alias = ui::model(source);
  registerValue(*fixture.scene, alias);
  CHECK(take(alias.value().snapshot()) == take(source.snapshot()));
  const auto before = take(source.snapshot());
  const auto whole = alias.value().set(Record{2, false});
  const auto field = alias.field(&Record::count).set(2);

  REQUIRE_FALSE(whole);
  REQUIRE_FALSE(field);
  CHECK(std::get<TreeError>(whole.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK(std::get<TreeError>(field.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK(take(source.snapshot()) == before);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
  const auto assigned = alias.field(&Record::count) = 3;

  REQUIRE_FALSE(assigned.result());
  CHECK(std::get<TreeError>(assigned.result().error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK(take(fixture.scene->diagnostics()).errors.size() == 1);
  const auto assignedWhole = alias.value() = Record{3, false};

  REQUIRE_FALSE(assignedWhole.result());
  CHECK(std::get<TreeError>(assignedWhole.result().error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK(take(fixture.scene->diagnostics()).errors.size() == 2);
  take(input.set(4));
  fixture.prepare();
  CHECK(take(alias.value().snapshot())->value == Record{4, true});
  CHECK(take(alias.value().snapshot())->revision == 1);
  CHECK_FALSE(fixture.scene->state(0));
}

TEST_CASE("alias qualification rejects foreign scenes and preserves external state after mount rollback") {
  Fixture first;
  Fixture second;
  auto external = take(first.scene->state(std::string("original")));
  auto source = take(first.scene->derive(external, [](const std::string &value) {
    return value;
  }));
  auto writable = ui::model(external);
  auto readonly = ui::model(source);
  const auto foreignState = ui::mount(*second.scene, ui::label(writable));
  const auto foreignSource = ui::mount(*second.scene, ui::label(readonly));

  REQUIRE_FALSE(foreignState);
  REQUIRE_FALSE(foreignSource);
  CHECK(std::get<TreeError>(foreignState.error().cause) == TreeError::WRONG_SESSION);
  CHECK(std::get<TreeError>(foreignSource.error().cause) == TreeError::WRONG_SESSION);
  ui::Model<int> invalid;
  const auto failed = ui::mount(*first.scene, ui::column(ui::label(writable), ui::label(invalid)));

  REQUIRE_FALSE(failed);
  CHECK(take(external.snapshot())->value == "original");
  CHECK(take(external.set("retained")).changed);
  ui::Ref<Label> caption;
  take(ui::mount(*first.scene, ui::label(caption, writable)));
  CHECK(take(caption.text().get()) == "retained");
  std::expected<std::shared_ptr<const PropertyChange<std::string>>, ManagedError> foreignRead;
  std::thread other([&] {
    foreignRead = writable.value().snapshot();
  });
  other.join();

  REQUIRE_FALSE(foreignRead);
  CHECK(std::get<TreeError>(foreignRead.error().cause) == TreeError::WRONG_THREAD);
}

TEST_CASE("read-only aliases preserve live origin checks and the existing derived last-good lifetime") {
  Fixture fixture;
  ui::Ref<Label> origin;
  take(ui::mount(*fixture.scene, ui::label(origin, "qualified")));
  auto live = take(fixture.scene->liveText(origin.captionText()));
  auto derived = take(fixture.scene->derive(live, [](const GraphemeText &value) {
    return std::string(value.bytes());
  }));
  auto directAlias = ui::model(live);
  auto derivedAlias = ui::model(derived);
  ui::Ref<Label> directCaption;
  ui::Ref<Label> derivedCaption;
  take(ui::mount(*fixture.scene,
                 ui::column(ui::label(directCaption, directAlias), ui::label(derivedCaption, derivedAlias))));
  CHECK(take(directCaption.text().get()) == "qualified");
  CHECK(take(derivedCaption.text().get()) == "qualified");
  take(fixture.scene->remove(origin.node()));
  const auto direct = directAlias.value().snapshot();
  const auto projected = take(derivedAlias.value().snapshot());

  REQUIRE_FALSE(direct);
  CHECK(std::get<TreeError>(direct.error().cause) == TreeError::STALE_NODE);
  CHECK(projected == take(derived.snapshot()));
  CHECK(projected->value == "qualified");
  const auto sameSource = directCaption.setModel(directAlias);

  REQUIRE_FALSE(sameSource);
  CHECK(std::get<TreeError>(sameSource.error().cause) == TreeError::STALE_NODE);
  const auto rejected = ui::mount(*fixture.scene, ui::label(ui::model(live)));

  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::STALE_NODE);
}

TEST_CASE("common scalar models replace owned writable and read-only caption sources and live text dependencies") {
  Fixture fixture;
  auto state = take(fixture.scene->state(2));
  auto derived = take(fixture.scene->derive(state, [](const int &value) {
    return value * 10;
  }));
  auto owned = ui::model(1);
  auto writable = ui::model(state);
  auto readonly = ui::model(derived);
  ui::Ref<Label> caption;
  ui::Ref<Label> formatted;
  take(ui::mount(*fixture.scene, ui::column(ui::label(caption, owned),
                                            ui::label(formatted, portals::text("{} / {}", writable, readonly)))));
  CHECK(take(formatted.text().get()) == "2 / 20");
  take(caption.setModel(writable));
  CHECK(take(caption.text().get()) == "2");
  take(caption.setModel(readonly));
  CHECK(take(caption.text().get()) == "20");
  take(state.set(3));
  fixture.prepare();
  fixture.prepare();
  CHECK(take(caption.text().get()) == "30");
  CHECK(take(formatted.text().get()) == "3 / 30");
  take(caption.setModel(owned));
  CHECK(take(caption.text().get()) == "1");
}

TEST_CASE("navigation current resolves cold in one mount and follows only accepted group switches") {
  Fixture fixture;
  auto a = ui::model(std::array{1, 2, 3});
  auto b = ui::model(std::array{10, 20, 30});
  auto rejected = ui::model(std::array{100, 200, 300}, CollectionLimits{}, ModelOptions{.max_consumers = 1});
  auto navigation = ui::navigation(a);
  auto current = navigation.current();
  ui::Ref<Label> caption;
  CHECK_FALSE(navigation.managed());
  CHECK_FALSE(current.value().snapshot());
  auto title = [](const NavigationPosition &value) {
    return std::to_string(value.resource.identity.instance);
  };
  take(ui::mount(*fixture.scene, ui::column(ui::label(caption, current, title), ui::virtualList(navigation),
                                            ui::virtualList(navigation))));
  auto channel = take(take(navigation.managed()).current());
  const auto initial = take(channel.snapshot());
  const auto initialTitle = take(caption.text().get());
  CHECK(take(current.value().snapshot()) == initial);
  CHECK(initialTitle == title(initial->value));
  const auto readonly = current.value().set(initial->value);

  REQUIRE_FALSE(readonly);
  CHECK(std::get<TreeError>(readonly.error().cause) == TreeError::INVALID_ARGUMENT);
  std::vector<ModelResource> observed(std::initializer_list<ModelResource>{});
  take(fixture.scene->observe(channel, [&](const NavigationPosition &value) {
    observed.push_back(value.resource);
  }));
  fixture.prepare();
  const auto failed = navigation.setModel(rejected);

  REQUIRE_FALSE(failed);
  CHECK(take(current.value().snapshot()) == initial);
  CHECK(take(caption.text().get()) == initialTitle);
  CHECK_FALSE(rejected.managed());
  take(navigation.setModel(b));
  const auto second = take(channel.snapshot());

  CHECK(second->value.resource != initial->value.resource);
  CHECK(take(current.value().snapshot()) == second);
  fixture.prepare();
  fixture.prepare();
  CHECK(take(caption.text().get()) == title(second->value));
  take(navigation.setModel(a));
  fixture.prepare();
  fixture.prepare();
  CHECK(take(current.value().snapshot())->value.resource == initial->value.resource);
  CHECK(take(caption.text().get()) == initialTitle);
  REQUIRE(observed.size() == 3);
  CHECK(observed[0] == initial->value.resource);
  CHECK(observed[1] == second->value.resource);
  CHECK(observed[2] == initial->value.resource);
  take(fixture.scene->close());
  CHECK_FALSE(current.value().snapshot());
}

TEST_CASE("cold navigation channel failure rolls back its owned attempt and aliases preserve external states") {
  Fixture fixture({.maxStates = 4});
  auto external = take(fixture.scene->state(5));
  auto navigation = ui::navigation(ui::model(std::array{1, 2}));
  auto current = navigation.current();
  const auto failed = ui::mount(*fixture.scene, ui::label(current, [](const NavigationPosition &) {
    return "requires one more output Property";
  }));

  REQUIRE_FALSE(failed);
  CHECK_FALSE(navigation.managed());
  CHECK_FALSE(current.value().snapshot());
  CHECK(take(external.set(6)).changed);
  take(fixture.scene->state(1));
  take(fixture.scene->state(2));
  take(fixture.scene->state(3));
  CHECK_FALSE(fixture.scene->state(4));
}

TEST_CASE("alias construction allocation failure is deferred without inspecting or retiring the source") {
  Fixture fixture;
  auto external = take(fixture.scene->state(5));
  const auto before = take(external.snapshot());
  admissionAllocationsBeforeFailure = 0;
  auto alias = ui::model(external);

  admissionAllocationsBeforeFailure = -1;
  const auto failure = ui::mount(*fixture.scene, ui::label(alias));

  REQUIRE_FALSE(failure);
  CHECK(std::get<TreeError>(failure.error().cause) == TreeError::NO_MEMORY);
  CHECK(take(external.snapshot()) == before);
  CHECK(take(external.set(6)).changed);
}

TEST_CASE("field addition publishes through the root and preserves unrelated fields") {
  Fixture fixture;
  auto source = ui::model(Record{10, true});
  auto state = registerValue(*fixture.scene, source);
  auto result = source.field(&Record::count) += 3;

  CHECK(take(result.result()).changed);
  const auto changed = take(source.value().snapshot());

  CHECK(changed->value == Record{13, true});
  CHECK_FALSE(take((source.field(&Record::count) += 0).result()).changed);
  auto readonly = ui::model(take(fixture.scene->derive(state, [](const Record &value) {
    return value;
  })));
  registerValue(*fixture.scene, readonly);
  const auto rejected = (readonly.field(&Record::count) += 1).result();

  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK(take(source.value().snapshot()) == changed);
}

TEST_CASE("button scalar captions preserve projections and replace complete text expressions") {
  Fixture fixture;
  auto first = ui::model(Record{4, true});
  auto second = ui::model(Record{7, false});
  ui::Ref<Button> button;

  take(ui::mount(*fixture.scene, ui::button(button, first, &Record::count)));
  CHECK(take(button.text().get()) == "4");
  take(button.setModel(second));
  CHECK(take(button.text().get()) == "7");
  second.field(&Record::count) += 1;
  fixture.prepare();
  fixture.prepare();
  CHECK(take(button.text().get()) == "8");
  take(button.setModel(text("{} units", first.field(&Record::count))));
  CHECK(take(button.text().get()) == "4 units");
  CHECK_FALSE(button.setModel(first));
  CHECK(take(button.text().get()) == "4 units");
  first.field(&Record::count) += 1;
  fixture.prepare();
  fixture.prepare();
  CHECK(take(button.text().get()) == "5 units");
}

TEST_CASE("live frame titles share value bindings and preserve geometry across replacement and resize") {
  Fixture fixture;
  auto source = ui::model(std::string("Short"));
  ui::Ref<Border> outer;

  take(ui::mount(*fixture.scene, ui::vbox(ui::label("body").framed(outer, "ascii", source).flexX().height(4))));
  auto initial = paintValueFrame(fixture, outer.node());

  CHECK(std::ranges::find(initial.text, "Short") != initial.text.end());
  SizePolicy before;

  take(fixture.app->tree().read(outer.node(), [&](const Border &border) {
    before = border.sizePolicy();
  }));
  source.value() = "A much longer changing title";
  auto changed = paintValueFrame(fixture, outer.node());

  CHECK(changed.top.find("A much longer changing title") != std::string::npos);
  auto clipped = paintValueFrame(fixture, outer.node(), {12, 12});

  CHECK(clipped.top.find("A much longer changing title") == std::string::npos);
  CHECK(clipped.top.find("A much lon") != std::string::npos);
  auto restored = paintValueFrame(fixture, outer.node());

  CHECK(restored.top.find("A much longer changing title") != std::string::npos);
  auto replacement = ui::model(std::string("Replacement"));

  take(outer.setModel(replacement));
  auto switched = paintValueFrame(fixture, outer.node());

  CHECK(std::ranges::find(switched.text, "Replacement") != switched.text.end());
  take(outer.setModel(text("{}!", source)));
  CHECK_FALSE(outer.setModel(replacement));
  auto formatted = paintValueFrame(fixture, outer.node());

  CHECK(std::ranges::find(formatted.text, "A much longer changing title!") != formatted.text.end());
  take(fixture.app->tree().read(outer.node(), [&](const Border &border) {
    CHECK(border.sizePolicy().width.minimum == before.width.minimum);
    CHECK(border.sizePolicy().width.maximum == before.width.maximum);
    CHECK(border.sizePolicy().height.minimum == before.height.minimum);
  }));
}

TEST_CASE("frame title admission and checked preparation preserve the last valid title on failure") {
  Fixture fixture;
  auto source = ui::model(std::string("Original"));
  ui::Ref<Border> outer;

  take(ui::mount(*fixture.scene, ui::label("body").framed(outer, "ascii", source).width(24).height(4)));
  paintValueFrame(fixture, outer.node());
  auto invalid = ui::model(std::string(4097, 'x'));
  const auto rejected = outer.setModel(invalid);

  REQUIRE_FALSE(rejected);
  CHECK_FALSE(invalid.value().snapshot());
  auto unchanged = paintValueFrame(fixture, outer.node());

  CHECK(std::ranges::find(unchanged.text, "Original") != unchanged.text.end());
  const auto candidate = take(GraphemeText::from("Not published"));
  admissionAllocationsBeforeFailure = 0;
  const auto failed = setBorderTitle(fixture.app->tree(), outer.node(), candidate);

  admissionAllocationsBeforeFailure = -1;
  REQUIRE_FALSE(failed);
  CHECK(failed.error() == TreeError::NO_MEMORY);
  auto preserved = paintValueFrame(fixture, outer.node());

  CHECK(std::ranges::find(preserved.text, "Original") != preserved.text.end());
  source.value() = "Still attached";
  auto updated = paintValueFrame(fixture, outer.node());

  CHECK(std::ranges::find(updated.text, "Still attached") != updated.text.end());
  take(fixture.scene->remove(outer.node()));
  CHECK_FALSE(outer.setModel(source));
}

TEST_CASE("localized live text resolves only explicit dependency changes and owns language context") {
  Fixture fixture;
  auto language = ui::model(false);
  auto population = ui::model(742);
  ui::Ref<Label> caption;
  int resolutions{};
  auto resolver = [&resolutions](std::string_view key, int,
                                 bool russian) -> std::expected<std::string, TextResourceError> {
    ++resolutions;
    if (key != "population") {
      return std::unexpected(TextResourceError::MISSING_RESOURCE);
    }
    return russian ? "Население: {0}" : "Population: {0}";
  };

  take(ui::mount(*fixture.scene, ui::label(caption, text(i18n("population"), resolver, population, language))));
  CHECK(take(caption.text().get()) == "Population: 742");
  const auto initial = resolutions;

  fixture.prepare();
  fixture.prepare();
  CHECK(resolutions == initial);
  language.value() = true;
  fixture.prepare();
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Население: 742");
  CHECK(resolutions == initial + 1);
  population.value() = 743;
  fixture.prepare();
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Население: 743");
  CHECK(resolutions == initial + 2);
}

TEST_CASE("localized text rejects missing resources and preserves the last valid runtime result") {
  Fixture fixture;
  auto source = ui::model(1);
  auto missing = ui::mount(*fixture.scene, ui::label(text(i18n("missing"), ui::TextResolver{}, source)));

  REQUIRE_FALSE(missing);
  CHECK(missing.error().resource == TextResourceError::RESOLVER_REQUIRED);
  CHECK_FALSE(source.value().snapshot());
  using Resolver = std::expected<std::string, TextResourceError> (*)(std::string_view, int);
  auto missingPointer =
    ui::mount(*fixture.scene, ui::label(text(i18n("missing"), static_cast<Resolver>(nullptr), source)));

  REQUIRE_FALSE(missingPointer);
  CHECK(missingPointer.error().resource == TextResourceError::RESOLVER_REQUIRED);
  CHECK_FALSE(source.value().snapshot());
  ui::Ref<Label> caption;
  auto resolver = [](std::string_view, int value) -> std::expected<std::string, TextResourceError> {
    if (value < 0) {
      return std::unexpected(TextResourceError::MISSING_RESOURCE);
    }

    return "Accepted {}";
  };

  take(ui::mount(*fixture.scene, ui::label(caption, text(i18n("value"), resolver, source))));
  CHECK(take(caption.text().get()) == "Accepted 1");
  source.value() = -1;
  const auto rejected = fixture.scene->prepare(*fixture.app, {40, 12});

  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == TreeError::INVALID_ARGUMENT);
  CHECK(take(caption.text().get()) == "Accepted 1");
  source.value() = 2;
  fixture.prepare();
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Accepted 2");
}

TEST_CASE("source metadata projects the accepted navigation resource before and after publication") {
  Fixture fixture;
  auto europe = ui::model(std::array{1, 2});
  auto asia = ui::model(std::array{10, 20});
  auto rejected = ui::model(std::array{100, 200}, CollectionLimits{}, ModelOptions{.max_consumers = 1});
  auto navigation = ui::navigation(europe);
  auto labels = ui::bySource<int>({{europe, "Europe"}, {asia, "Asia"}, {rejected, "Rejected"}});
  ui::Ref<Label> caption;

  take(ui::mount(*fixture.scene, ui::column(ui::label(caption, navigation.current(), labels),
                                            ui::virtualList(navigation), ui::virtualList(navigation))));
  CHECK(take(caption.text().get()) == "Europe");
  CHECK_FALSE(asia.managed());
  CHECK_FALSE(rejected.managed());
  CHECK_FALSE(navigation.setModel(rejected));
  fixture.prepare();
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Europe");
  CHECK_FALSE(rejected.managed());
  take(navigation.setModel(asia));
  fixture.prepare();
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Asia");
  take(navigation.setModel(europe));
  fixture.prepare();
  fixture.prepare();
  CHECK(take(caption.text().get()) == "Europe");
  const auto unknown = labels(NavigationPosition{});

  REQUIRE_FALSE(unknown);
  CHECK(unknown.error() == TreeError::INVALID_ARGUMENT);
  std::expected<std::string, TreeError> foreign;
  const auto value = take(take(take(navigation.managed()).current()).snapshot())->value;
  std::thread worker([&] {
    foreign = labels(value);
  });

  worker.join();
  REQUIRE_FALSE(foreign);
  CHECK(foreign.error() == TreeError::WRONG_THREAD);
  take(fixture.scene->close());
  CHECK(labels(value).error() == TreeError::CLOSED);
}

TEST_CASE("source metadata is typed owned and bounded without registering unselected sources") {
  Fixture fixture;
  auto first = ui::model(std::array{1});
  auto second = ui::model(std::array{2});
  auto navigation = ui::navigation(first);
  auto values = ui::bySource<int, Record>({{first, {3, true}}, {second, {5, false}}});
  ui::Ref<Label> caption;

  take(
    ui::mount(*fixture.scene, ui::label(caption, navigation.current(),
                                        [values](const NavigationPosition &position) -> std::expected<int, TreeError> {
                                          auto value = values(position);
                                          if (!value) {
                                            return std::unexpected(value.error());
                                          }

                                          return value->count;
                                        })));
  CHECK(take(caption.text().get()) == "3");
  CHECK_FALSE(second.managed());
  const auto position = take(take(take(navigation.managed()).current()).snapshot())->value;
  auto duplicate = ui::bySource<int>({{first, "A"}, {first, "B"}});
  auto oversized = ui::bySource<int>({{first, "A"}, {second, "B"}}, {.maxRows = 1});

  CHECK(duplicate(position).error() == TreeError::INVALID_ARGUMENT);
  CHECK(oversized(position).error() == TreeError::NODE_LIMIT);
}

TEST_CASE("pending and empty domain values remain distinct while captions retain the last valid value") {
  Fixture fixture;
  auto state = ui::model(NavigationPosition{.resolution = PositionResolution::PENDING});
  ui::Ref<Label> caption;

  take(ui::mount(*fixture.scene, ui::label(caption, state, [](const NavigationPosition &value) {
    return value.resolution == PositionResolution::PENDING ? "Loading" : "No current item";
  })));
  CHECK(take(caption.text().get()) == "Loading");
  state.value() = NavigationPosition{.resolution = PositionResolution::EMPTY};
  fixture.prepare();
  fixture.prepare();
  CHECK(take(caption.text().get()) == "No current item");
  ui::Model<NavigationPosition> unavailable;

  CHECK_FALSE(caption.setModel(unavailable));
  CHECK(take(caption.text().get()) == "No current item");
}

// This fixture exposes hierarchical identity directly. It has no total row count or flattened child array.
class LazyHierarchy final : public PositionResolver {
public:
  static constexpr RowId LEAF{900000000001ULL, 1};
  static constexpr RowId SIBLING{900000000002ULL, 1};
  static constexpr ModelResource RESOURCE{{901, 1}, 1};
  static constexpr SourceStamp SESSION{902, 1};

  std::expected<PositionReply, SourceError> resolve(const PositionRequest &request,
                                                    std::stop_token stop = {}) const override {
    if (stop.stop_requested()) {
      return std::unexpected(SourceError::CANCELLED);
    }
    if (request.session != SESSION || request.revision != revision) {
      return std::unexpected(SourceError::REVISION_CHANGED);
    }

    PositionReply result{RESOURCE, SESSION, revision, request.sequence};

    if (request.bookmark.resource() != RESOURCE) {
      result.state = PositionResolution::INCOMPATIBLE;

      return result;
    }
    ++identityLookups;
    const auto requested = request.bookmark.position();
    if (requested.kind != PositionKind::ITEM) {
      result.state = PositionResolution::UNSUPPORTED;
    } else if (requested.row == LEAF) {
      result.state = deleted ? PositionResolution::NEAREST : PositionResolution::EXACT;
      result.position = {PositionKind::ITEM, deleted ? SIBLING : LEAF, 0, revision, requested.affinity};
    } else {
      // The child exists in the logical hierarchy but its bounded identity page is not resident.
      result.state = PositionResolution::PENDING;
    }

    return result;
  }

  void moveLeaf() {
    parent = 7;
    ++revision;
  }

  void collapseParent() {
    collapsed = true;
  }

  void eraseLeaf() {
    deleted = true;
    ++revision;
  }

  std::uint64_t revision{1};
  std::uint64_t parent{3};
  bool collapsed{};
  bool deleted{};
  mutable std::size_t identityLookups{};
};

TEST_CASE("hierarchical bookmarks survive move and collapse with bounded nearest and pending resolution") {
  LazyHierarchy provider;
  const auto bookmark =
    take(ModelBookmark::create(LazyHierarchy::RESOURCE, {PositionKind::ITEM, LazyHierarchy::LEAF, 0, 1}));
  auto request = PositionRequest{LazyHierarchy::SESSION, 1, 1, bookmark};
  auto exact = take(provider.resolve(request));

  CHECK(positionReplyMatches(request, exact));
  CHECK(exact.state == PositionResolution::EXACT);
  CHECK(exact.position.row == LazyHierarchy::LEAF);
  provider.moveLeaf();
  request.revision = provider.revision;
  ++request.sequence;
  auto moved = take(provider.resolve(request));

  CHECK(positionReplyMatches(request, moved));
  CHECK_FALSE(positionReplyMatches(request, exact));
  CHECK(moved.state == PositionResolution::EXACT);
  CHECK(moved.position.row == LazyHierarchy::LEAF);
  provider.collapseParent();
  auto collapsed = take(provider.resolve(request));

  CHECK(collapsed.state == PositionResolution::EXACT);
  CHECK(collapsed.position.row == LazyHierarchy::LEAF);
  provider.eraseLeaf();
  request.revision = provider.revision;
  ++request.sequence;
  auto nearest = take(provider.resolve(request));

  CHECK(positionReplyMatches(request, nearest));
  CHECK(nearest.state == PositionResolution::NEAREST);
  CHECK(nearest.position.row == LazyHierarchy::SIBLING);
  request.bookmark =
    take(ModelBookmark::create(LazyHierarchy::RESOURCE, {PositionKind::ITEM, RowId{800000000001ULL, 1}, 0, 1}));
  auto pending = take(provider.resolve(request));

  CHECK(positionReplyMatches(request, pending));
  CHECK(pending.state == PositionResolution::PENDING);
  CHECK(provider.identityLookups == 5);
  CHECK(bookmark.metadata().empty());
  CHECK(bookmark.storageBytes() == sizeof(ModelBookmark));
  std::stop_source cancelled;

  cancelled.request_stop();
  CHECK(provider.resolve(request, cancelled.get_token()).error() == SourceError::CANCELLED);
  CHECK(provider.identityLookups == 5);
}

TEST_CASE("source catalogs reject ambiguous resource aliases without requiring metadata equality") {
  Fixture fixture;
  auto first = ui::model(std::array{1});
  auto second = ui::model(std::array{2});
  ui::detail::MountContext context;
  auto a = take(ui::detail::ModelAccess::resolve(*fixture.scene, first, context));
  auto b = take(ui::detail::ModelAccess::resolve(*fixture.scene, second, context));
  const auto resource = take(ModelResource::create());
  const auto session = resource.identity;
  auto navigation = take(fixture.scene->navigation(a, resource, session));

  take(fixture.scene->navigation(b, resource, session));
  ui::detail::ModelAccess::publish(context);
  auto projection = ui::bySource<int>({{first, "A"}, {second, "B"}});
  const auto position = take(take(navigation.current()).snapshot())->value;

  CHECK(projection(position).error() == TreeError::INVALID_ARGUMENT);
}

TEST_CASE("source catalog follows committed group selection despite notification backpressure") {
  Fixture fixture({}, 1);
  auto a = ui::model(std::array{1, 2});
  auto b = ui::model(std::array{3, 4});
  auto navigation = ui::navigation(a);
  auto catalog = ui::bySource<int>({{a, "A"}, {b, "B"}});
  ui::Ref<Label> caption;

  take(ui::mount(*fixture.scene,
                 ui::column(ui::label(caption, navigation.current(), catalog), ui::virtualList(navigation))));
  auto channel = take(take(navigation.managed()).current());

  take(fixture.scene->observe(channel, [](const NavigationPosition &) {
  }));
  take(fixture.scene->observe(channel, [](const NavigationPosition &) {
  }));
  fixture.prepare();
  take(navigation.setModel(b));
  CHECK(b.managed());
  CHECK(take(catalog(take(channel.snapshot())->value)) == "B");
  CHECK_FALSE(take(fixture.scene->diagnostics()).errors.empty());
  fixture.prepare();
  fixture.prepare();
  CHECK(take(caption.text().get()) == "B");
}

TEST_CASE("source catalog rejects a mixture of registered scene owners") {
  Fixture first;
  Fixture second;
  auto a = ui::model(std::array{1});
  auto b = ui::model(std::array{2});
  auto navA = ui::navigation(a);
  auto navB = ui::navigation(b);

  take(ui::mount(*first.scene, ui::virtualList(navA)));
  take(ui::mount(*second.scene, ui::virtualList(navB)));
  auto catalog = ui::bySource<int>({{a, "A"}, {b, "B"}});
  auto position = take(take(take(navA.managed()).current()).snapshot());

  CHECK(catalog(position->value).error() == TreeError::WRONG_SESSION);
}

TEST_CASE("caption replacement accepts the committed source even when its earlier notification was rejected") {
  Fixture fixture({}, 1);
  auto first = ui::model(Record{1, true});
  auto second = ui::model(Record{10, false});
  auto selected = first;
  ui::Ref<Label> caption;

  take(ui::mount(*fixture.scene,
                 ui::column(ui::label(caption, first, &Record::count), ui::label(second, &Record::count))));
  ui::detail::MountContext context;
  auto state = take(ui::detail::ValueModelAccess::resolve(*fixture.scene, second, context));
  ui::detail::ModelAccess::publish(context);
  take(fixture.scene->observe(state, [](const Record &) {
  }));
  take(fixture.scene->observe(state, [](const Record &) {
  }));
  fixture.prepare();
  const auto committed = take(second.field(&Record::count).set(11));

  REQUIRE(committed.notification_error);
  const bool admitted = caption.model(second);
  if (admitted) {
    selected = second;
  }

  REQUIRE(admitted);
  CHECK(take(caption.text().get()) == "11");
  selected.field(&Record::count) += 1;
  CHECK(take(second.value().snapshot())->value.count == 12);
  CHECK(take(first.value().snapshot())->value.count == 1);
}

TEST_CASE("source catalogs retain deferred allocation causes and reject an unqualified resource") {
  auto failed = ui::detail::ModelAccess::failed<int>(TreeError::NO_MEMORY);
  auto anotherFailed = ui::detail::ModelAccess::failed<int>(TreeError::NO_MEMORY);
  auto single = ui::bySource<int>({{failed, "failed"}});
  auto two = ui::bySource<int>({{failed, "first"}, {anotherFailed, "second"}});
  const auto resource = take(ModelResource::create());
  const auto qualified = NavigationPosition{.resource = resource};

  CHECK(single(qualified).error() == TreeError::NO_MEMORY);
  CHECK(two(qualified).error() == TreeError::NO_MEMORY);
  Fixture fixture;
  auto rows = ui::model(std::array{1});
  ui::detail::MountContext context;

  take(ui::detail::ModelAccess::resolve(*fixture.scene, rows, context));
  ui::detail::ModelAccess::publish(context);
  auto unqualified = ui::bySource<int>({{rows, "not a navigation resource"}});
  const auto rejected = unqualified(NavigationPosition{});

  CHECK_FALSE(rejected);
  if (!rejected) {
    CHECK(rejected.error() == TreeError::INVALID_ARGUMENT);
  }
}
} // namespace
