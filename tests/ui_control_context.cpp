// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/detail/control_context.hpp>
#include <portals/ui_control_context.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace {
using namespace portals;
using namespace portals::ui;
using Code = ControlContextErrorCode;

constexpr ControlPropertyDescriptor FOUR_FIELD_DESCRIPTOR{"text", ControlPropertyKind::TEXT, 1, {}};
static_assert(FOUR_FIELD_DESCRIPTOR.aliases.empty());
static_assert(
  std::same_as<decltype(ui::detail::controlSettingsPresence(std::declval<const ControlSettings &>())), std::uint32_t>);
static_assert(ui::detail::CONTROL_PRESENCE_BITS >= 32);
static_assert(ui::detail::CONTROL_PRESENCE_BITS >= ui::detail::CONTROL_PROPERTY_COUNT);

// The pre-E10 positional aggregate prefix must still end at the existing dialog member.
static_assert(requires {
  ControlSettings{{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
                  {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
                  {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, ControlDialogSettings{}};
});

// The complete pre-Notifications positional aggregate prefix still ends with MessageBox settings.
static_assert(requires {
  ControlSettings{{}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, {},
                  {}, ControlMessageSettings{}};
});

void check(bool condition, std::string_view message, std::source_location location = std::source_location::current()) {
  INFO("Caller line: ", location.line());
  REQUIRE_MESSAGE(condition, message);
}

template <class T>
T take(std::expected<T, ControlContextError> result, std::source_location location = std::source_location::current()) {
  check(result.has_value(), "expected success", location);

  if constexpr (!std::is_void_v<T>) {
    return *result;
  }
}

template <class T>
T take(std::expected<T, ManagedError> result, std::source_location location = std::source_location::current()) {
  check(result.has_value(), "expected managed success", location);

  return std::move(*result);
}

template <class T>
void fails(std::expected<T, ControlContextError> result, Code code,
           std::source_location location = std::source_location::current()) {
  check(!result && result.error().code == code, "expected exact typed failure", location);
}

void sparsePrecedence() {
  ControlSettings base;
  base.history.maxOperations = 20;
  base.history.maxBytes = 200;
  base.readOnly = true;
  ControlSettings parent;
  parent.history.groupTyping = false;
  ControlSettings definition;
  definition.history.maxOperations = 30;
  ControlSettings variantDefaults;
  variantDefaults.history.maxBytes = 0;
  ControlSettings instance;
  instance.history.maxOperations = DEFAULT_TEXT_HISTORY_OPERATIONS;
  instance.readOnly = false;

  ControlContext context;
  check(&context.defaults("LineEdit", base) == &context, "fluent same reference");
  context.define("OtherEdit", "LineEdit", parent).define("MyEdit", "OtherEdit", definition);
  context.defaults("MyEdit", variantDefaults);
  auto resolved = take(context.resolve("MyEdit", instance));
  check(resolved.type == "MyEdit" && resolved.base == "LineEdit", "named concrete base");
  check(resolved.lineage == std::vector<std::string>{"OtherEdit", "LineEdit"}, "closest ancestor first, no self");
  check(resolved.settings.history.maxOperations == DEFAULT_TEXT_HISTORY_OPERATIONS, "builtin-equal explicit override");
  check(resolved.settings.history.maxBytes == 0, "explicit zero preserved");
  check(resolved.settings.history.groupTyping == false && resolved.settings.readOnly == false, "false preserved");
  check(resolved.materializedLeaves == 4, "unique scalar leaves counted");

  auto copy = context;
  context.define("Later", "LineEdit");
  take(context.resolve("Later"));
  fails(copy.resolve("Later"), Code::UNKNOWN_TYPE);
  auto other = copy;
  other.define("CopyOnly", "Button");
  fails(context.resolve("CopyOnly"), Code::UNKNOWN_TYPE);
  take(copy.resolve("MyEdit"));

  auto temporary = ControlContext{}.define("Temporary", "LineEdit").defaults("Temporary", {.readOnly = false});
  check(take(temporary.resolve("Temporary")).settings.readOnly == false, "rvalue fluent context");
}

TEST_CASE("Canonical control property aliases retain the aggregate prefix and exact spelling") {
  const auto schema = controlPropertySchema("LineEdit");
  const auto placeholder = std::ranges::find(schema, std::string_view("placeholder"), &ControlPropertyDescriptor::path);
  REQUIRE(placeholder != schema.end());
  REQUIRE(placeholder->aliases.size() == 1);
  CHECK(placeholder->aliases.front() == "placeHolder");
  CHECK(ui::detail::ControlContextAccess::canonicalProperty("placeholder") == "placeholder");
  CHECK(ui::detail::ControlContextAccess::canonicalProperty("placeHolder") == "placeholder");
  const std::string_view wrongCaseInput{"PlaceHolder"};
  const auto wrongCase = ui::detail::ControlContextAccess::canonicalProperty(wrongCaseInput);
  CHECK(wrongCase == "PlaceHolder");
  CHECK(wrongCase.data() == wrongCaseInput.data());
}

TEST_CASE("Complete control presence preserves sparse values and the legacy low bits") {
  ControlSettings settings;
  settings.width = 0;
  settings.readOnly = false;
  settings.history.maxBytes = 0;
  settings.grid.scroll = false;

  const auto presence = ui::detail::controlSettingsPresenceSet(settings);
  const std::uint32_t legacy = ui::detail::controlSettingsPresence(settings);
  const ui::detail::ControlPropertyPresence legacyHighBits{0xc0000000ULL};
  ui::detail::ControlPropertyPresence materialized;

  materialized.set(1).set(8);

  CHECK(presence.count() == 4);
  CHECK(presence.test(1));
  CHECK(presence.test(8));
  CHECK(presence.test(14));
  CHECK(presence.test(29));
  CHECK(legacy == 0x20004102U);
  CHECK((presence & ~materialized).count() == 2);
  CHECK(legacyHighBits.test(30));
  CHECK(legacyHighBits.test(31));
  CHECK((presence & ~legacyHighBits) == presence);
  CHECK(ui::detail::controlSettingsPresenceSet(ControlSettings{}).none());
}

void graphsAndStickyErrors() {
  ControlContext forward;
  forward.define("A", "B").define("B", "LineEdit");
  take(forward.validate());
  check(take(forward.resolve("A")).lineage == std::vector<std::string>{"B", "LineEdit"}, "forward reference");

  ControlContext cycle;
  cycle.define("A", "B").define("B", "A");
  fails(cycle.validate(), Code::INHERITANCE_CYCLE);
  fails(cycle.resolve("Button"), Code::INHERITANCE_CYCLE);

  ControlContext missing;
  missing.define("A", "Absent");
  fails(missing.validate(), Code::UNKNOWN_TYPE);
  ControlContext shadow;
  shadow.define("Button", "LineEdit");
  fails(shadow.validate(), Code::BUILTIN_SHADOW);
  shadow.define("Valid", "Button");
  fails(shadow.resolve("Valid"), Code::BUILTIN_SHADOW);

  ControlContext duplicate;
  duplicate.define("A", "Button").define("A", "LineEdit");
  fails(duplicate.validate(), Code::DUPLICATE_TYPE);
  ControlContext repeatedDefaults;
  repeatedDefaults.defaults("Button", {}).defaults("Button", {});
  fails(repeatedDefaults.validate(), Code::DUPLICATE_PROPERTY);
  ControlContext unknownDefaults;
  unknownDefaults.defaults("Absent", {});
  fails(unknownDefaults.validate(), Code::UNKNOWN_TYPE);
  ControlContext badName;
  badName.define("not-an-identifier", "Button");
  fails(badName.validate(), Code::INVALID_NAME);
}

void schemasAndValues() {
  ControlContext context;
  check(controlPropertySchema("Absent").empty(), "unknown builtin schema absent");
  const auto editSchema = controlPropertySchema("LineEdit");
  check(!editSchema.empty(), "LineEdit schema available");
  bool enumFound = false;

  for (const auto &descriptor : editSchema) {
    if (descriptor.path == "editMode") {
      enumFound = descriptor.enumNames.size() == 3 && descriptor.enumNames[0] == "IMMEDIATE";
    }
  }

  check(enumFound, "enum descriptor vocabulary");
  fails(context.resolve("Button", {.history = {.maxOperations = 20}}), Code::UNSUPPORTED_PROPERTY);
  fails(context.resolve("Label", {.gap = 1}), Code::UNSUPPORTED_PROPERTY);
  fails(context.resolve("VBox", {.text = text("no")}), Code::UNSUPPORTED_PROPERTY);
  fails(context.resolve("Button", {.width = -1}), Code::INVALID_VALUE);
  fails(context.resolve("Button", {.flex = 0}), Code::INVALID_VALUE);
  fails(context.resolve("CheckBox", {.state = static_cast<CheckState>(100)}), Code::INVALID_VALUE);
  fails(context.resolve("CheckBox", {.cycle = static_cast<CheckCycle>(100)}), Code::INVALID_VALUE);
  fails(context.resolve("LineEdit", {.editMode = static_cast<EditMode>(100)}), Code::INVALID_VALUE);
  fails(context.resolve("LineEdit", {.programmaticChange = static_cast<ProgrammaticChange>(100)}), Code::INVALID_VALUE);
  fails(context.resolve("LineEdit", {.displayMode = static_cast<LineEditDisplayMode>(100)}), Code::INVALID_VALUE);
  take(context.resolve("VBox", {.width = 0, .height = 0, .gap = 0}));
  take(context.resolve("LineEdit", {.history = {.enabled = false, .maxOperations = 0, .maxBytes = 0}}));

  constexpr std::array<std::string_view, 4> PATHS{"text", "width", "height", "flex"};
  context.registerType("Custom", PATHS).define("CustomAlias", "Custom", {.text = i18n("caption")});
  const auto custom = take(context.resolve("CustomAlias"));
  check(custom.base == "Custom" && custom.settings.text->resourceKey() == "caption", "explicit custom schema");
  fails(context.resolve("CustomAlias", {.readOnly = true}), Code::UNSUPPORTED_PROPERTY);
  constexpr std::array<std::string_view, 1> UNKNOWN{"notKnown"};
  ControlContext unknownSchema;
  unknownSchema.registerType("Custom", UNKNOWN);
  fails(unknownSchema.validate(), Code::UNKNOWN_PROPERTY);
  constexpr std::array<std::string_view, 2> REPEATED{"text", "text"};
  ControlContext duplicateSchema;
  duplicateSchema.registerType("Custom", REPEATED);
  fails(duplicateSchema.validate(), Code::DUPLICATE_PROPERTY);
}

void threeState() {
  ControlContext context;
  context.defaults("CheckBox", {.cycle = CheckCycle::TWO_STATE});
  const auto resolved = take(context.resolve("ThreeState"));
  check(resolved.base == "ThreeState" && resolved.lineage == std::vector<std::string>{"CheckBox"},
        "ThreeState identity");
  check(resolved.settings.cycle == CheckCycle::THREE_STATE, "ThreeState overrides CheckBox defaults");
  context.define("MyThree", "ThreeState", {.state = CheckState::MIXED});
  check(take(context.resolve("MyThree")).settings.cycle == CheckCycle::THREE_STATE, "fixed cycle inherited");
  fails(context.resolve("MyThree", {.cycle = CheckCycle::TWO_STATE}), Code::FIXED_CYCLE);
  ControlContext definition;
  definition.define("Bad", "ThreeState", {.cycle = CheckCycle::TWO_STATE});
  fails(definition.validate(), Code::FIXED_CYCLE);
  ControlContext defaults;
  defaults.defaults("ThreeState", {.cycle = CheckCycle::TWO_STATE});
  fails(defaults.validate(), Code::FIXED_CYCLE);
}

void limits() {
  ControlContext definitions({.maxDefinitions = 1});
  definitions.define("A", "Button").define("B", "Button");
  fails(definitions.validate(), Code::DEFINITION_LIMIT);
  ControlContext inherited({.maxInheritance = 1});
  inherited.define("A", "Button");
  take(inherited.resolve("A"));
  inherited.define("B", "A");
  fails(inherited.validate(), Code::INHERITANCE_LIMIT);
  ControlContext depth({.maxStructDepth = 1});
  fails(depth.resolve("LineEdit", {.history = {.enabled = false}}), Code::STRUCT_DEPTH_LIMIT);
  ControlContext materialized({.maxMaterializedLeaves = 1});
  materialized.defaults("Button", {.width = 1});
  take(materialized.resolve("Button", {.width = 0}));
  fails(materialized.resolve("Button", {.height = 0}), Code::MATERIALIZED_LEAF_LIMIT);
  ControlContext zero({.maxDefinitions = 0});
  fails(zero.validate(), Code::INVALID_LIMIT);
  ControlContext unsupportedIdentity({.maxInheritance = 33});
  fails(unsupportedIdentity.validate(), Code::INVALID_LIMIT);
  ControlContext boundary;

  for (int index = 0; index < 32; ++index) {
    boundary.define("T" + std::to_string(index), index ? "T" + std::to_string(index - 1) : "Button");
  }

  check(take(boundary.resolve("T31")).lineage.size() == 32, "exact ancestor boundary");
  boundary.define("TooDeep", "T31");
  fails(boundary.validate(), Code::INHERITANCE_LIMIT);
}

void provenanceAndConcurrency() {
  auto source = std::make_shared<ControlSourceOrigin>();
  source->sourceIdentity = "theme.scene";
  source->byteOffset = 41;
  source->line = 3;
  source->column = 7;
  ControlSettings inherited;
  inherited.history.enabled = false;
  inherited.origin = source;
  ControlContext bad;
  bad.define("BadButton", "Button", inherited).define("Descendant", "BadButton");
  auto failure = bad.resolve("Descendant");
  fails(failure, Code::UNSUPPORTED_PROPERTY);
  check(failure.error().diagnostic && failure.error().diagnostic->path == "BadButton.history.enabled",
        "inherited path");
  const auto origin = failure.error().diagnostic->origin;
  check(origin && origin->sourceIdentity == "theme.scene" && origin->byteOffset == 41 && origin->line == 3 &&
          origin->column == 7,
        "inherited exact source coordinates");

  ControlSettings patch;
  patch.text = i18n("owned.key");
  patch.placeholder = i18n("placeholder.key");
  patch.origin = source;
  ControlContext good;
  good.define("Edit", "LineEdit", patch);
  patch.text = i18n("changed.key");
  auto resolved = take(good.resolve("Edit"));
  check(resolved.settings.text->resourceKey() == "owned.key", "TextValue copied during registration");
  check(resolved.settings.origins.size() == 2, "effective text and placeholder origins retained");
  check(resolved.settings.origins.front().source == source, "effective provenance owns source");
  ControlSettings instance;
  instance.text = i18n("override");
  resolved = take(good.resolve("Edit", instance));
  check(!resolved.settings.origins.front().source && resolved.settings.origins.back().source == source,
        "winning source replaced without dropping other source");

  const auto snapshot = good;
  std::array<bool, 2> passed{};
  std::thread first([&] {
    passed[0] = snapshot.resolve("Edit").has_value();
  });
  std::thread second([&] {
    passed[1] = snapshot.resolve("Edit").has_value();
  });
  first.join();
  second.join();
  check(passed[0] && passed[1], "concurrent const reads");
}
} // namespace

TEST_CASE("ControlContext sparse precedence and independent snapshots") {
  sparsePrecedence();
}

TEST_CASE("ControlContext validates complete graphs and sticky errors") {
  graphsAndStickyErrors();
}

TEST_CASE("ControlContext validates schemas, ranges, enums and explicit custom types") {
  schemasAndValues();
}

TEST_CASE("ControlContext fixes ThreeState cycle after CheckBox defaults") {
  threeState();
}

TEST_CASE("ControlContext enforces finite bounds before effective materialization") {
  limits();
}

TEST_CASE("ControlContext retains inherited provenance and supports concurrent const snapshots") {
  provenanceAndConcurrency();
}

TEST_CASE("Prepared ControlContext snapshots invalidate qualification on every mutation") {
  ControlContext original;
  original.define("Edit", "LineEdit", {.readOnly = true});

  auto prepared = take(original.prepared());
  const auto snapshot = prepared;
  prepared.define("Broken", "Absent");
  fails(prepared.validate(), Code::UNKNOWN_TYPE);
  take(snapshot.validate());
  check(take(snapshot.resolve("Edit")).settings.readOnly == true, "qualified copy owns original immutable data");
  fails(snapshot.resolve("Broken"), Code::UNKNOWN_TYPE);

  auto changedDefaults = snapshot;
  changedDefaults.defaults("Edit", {.gap = 1});
  fails(changedDefaults.validate(), Code::UNSUPPORTED_PROPERTY);

  auto changedSchema = snapshot;
  const std::array<std::string_view, 1> paths{"text"};
  changedSchema.registerType("Button", paths);
  fails(changedSchema.validate(), Code::BUILTIN_SHADOW);
  take(snapshot.validate());
}

TEST_CASE("ControlContext exact custom schemas and nondefault bounds are observable") {
  ControlContext context;
  check(context.empty(), "default context permits legacy fast path");
  check(!context.hasRegisteredType("LineEdit"), "builtin is not an explicitly registered custom schema");

  const std::array<std::string_view, 2> paths{"text", "width"};
  const std::array<std::string_view, 2> reversed{"width", "text"};
  const std::array<std::string_view, 2> duplicate{"text", "text"};
  const std::array<std::string_view, 2> unknown{"text", "notAProperty"};
  const std::array<std::string_view, 1> subset{"text"};
  context.registerType("Custom", paths).define("Variant", "Custom");
  check(!context.empty(), "registered schema prevents legacy bypass");
  check(context.hasRegisteredType("Custom") && !context.hasRegisteredType("Variant"), "exact custom type query");
  check(context.matchesRegisteredType("Custom", reversed), "schema ordering does not matter");
  check(!context.matchesRegisteredType("Custom", subset), "subset is not exact");
  check(!context.matchesRegisteredType("Custom", duplicate), "duplicate paths rejected");
  check(!context.matchesRegisteredType("Custom", unknown), "unknown paths rejected");
  check(!context.matchesRegisteredType("Missing", paths), "missing schema rejected");

  ControlContextLimits limits;
  limits.maxMaterializedLeaves = 1;
  check(!ControlContext(limits).empty(), "custom whole-mount budget cannot bypass validation");
}

TEST_CASE("Local defaults preserve inherited sparse leaves and original snapshots") {
  auto inheritedSource = std::make_shared<ControlSourceOrigin>();
  inheritedSource->sourceIdentity = "parent.scene";
  auto localSource = std::make_shared<ControlSourceOrigin>();
  localSource->sharedSourceIdentity = std::make_shared<const std::string>("local.scene");
  ControlSettings inherited;
  inherited.readOnly = true;
  inherited.history.maxOperations = 20;
  inherited.origin = inheritedSource;
  ControlContext context;
  context.defaults("LineEdit", inherited);

  auto local = take(context.prepared());
  ControlSettings patch;
  patch.readOnly = false;
  patch.origin = localSource;
  local.overlayDefaults("LineEdit", patch);
  const auto resolved = take(local.resolve("LineEdit"));
  check(resolved.settings.readOnly == false, "local false overrides inherited true");
  check(resolved.settings.history.maxOperations == 20, "absent local leaf preserves parent");
  check(take(context.resolve("LineEdit")).settings.readOnly == true, "parent snapshot remains unchanged");
  bool localFound = false;
  bool inheritedFound = false;
  for (const auto &origin : resolved.settings.origins) {
    localFound |= origin.path == "readOnly" && origin.source == localSource;
    inheritedFound |= origin.path == "history.maxOperations" && origin.source == inheritedSource;
  }
  check(localFound && inheritedFound, "each winning leaf keeps its original source");

  patch.readOnly = true;
  auto sameValue = context;
  sameValue.overlayDefaults("LineEdit", patch);
  const auto sameResolved = take(sameValue.resolve("LineEdit"));
  const auto sameOrigin =
    std::ranges::find(sameResolved.settings.origins, std::string("readOnly"), &ControlPropertyOrigin::path);
  check(sameOrigin != sameResolved.settings.origins.end() && sameOrigin->source == localSource,
        "same-value override still changes winning provenance");

  local.overlayDefaults("NewEdit", {.readOnly = false}).define("NewEdit", "LineEdit");
  take(local.validate());
  auto invalid = take(local.prepared());
  invalid.overlayDefaults("LineEdit", {.gap = 1});
  fails(invalid.validate(), Code::UNSUPPORTED_PROPERTY);
}

TEST_CASE("A valid descendant patch cannot hide an invalid ancestor declaration") {
  ControlContext context;
  context.define("Invalid", "Button", {.cycle = CheckCycle::TWO_STATE})
    .define("Descendant", "Invalid", {.cycle = CheckCycle::THREE_STATE});
  fails(context.resolve("Descendant"), Code::UNSUPPORTED_PROPERTY);
  fails(context.prepared(), Code::UNSUPPORTED_PROPERTY);
}

TEST_CASE("Graph errors retain the declaration that owns the invalid inheritance edge") {
  auto first = std::make_shared<ControlSourceOrigin>();
  first->sourceIdentity = "first.scene";
  first->line = 3;
  auto second = std::make_shared<ControlSourceOrigin>();
  second->sourceIdentity = "second.scene";
  second->line = 7;

  const auto verify = [](const ControlContext &context, Code code, std::string_view path,
                         const std::shared_ptr<const ControlSourceOrigin> &origin) {
    const auto result = context.validate();
    fails(result, code);
    REQUIRE(result.error().diagnostic);
    CHECK(result.error().diagnostic->path == path);
    CHECK(result.error().diagnostic->origin == origin);
  };

  ControlContext missing;
  missing.define("A", "Absent", {.origin = first});
  verify(missing, Code::UNKNOWN_TYPE, "A.base", first);

  ControlContext defaults;
  defaults.defaults("Absent", {.origin = first});
  verify(defaults, Code::UNKNOWN_TYPE, "Absent", first);

  ControlContext cycle;
  cycle.define("A", "B", {.origin = first}).define("B", "A", {.origin = second});
  verify(cycle, Code::INHERITANCE_CYCLE, "B.base", second);

  ControlContext limited({.maxInheritance = 1});
  limited.define("A", "B", {.origin = first}).define("B", "Button", {.origin = second});
  verify(limited, Code::INHERITANCE_LIMIT, "B.base", second);
}

TEST_CASE("Layout identity includes natural C++ ancestors within the validated inheritance bound") {
  ControlContext context;
  check(take(context.resolve("VBox")).lineage == std::vector<std::string>{"Column"}, "VBox retains Column");
  check(take(context.resolve("HBox")).lineage == std::vector<std::string>{"Row"}, "HBox retains Row");
  context.define("Dense", "VBox");
  check(take(context.resolve("Dense")).lineage == std::vector<std::string>{"VBox", "Column"}, "named layout ancestry");

  ControlContext sameName;
  sameName.define("Column", "VBox");
  check(take(sameName.resolve("Column")).lineage == std::vector<std::string>{"VBox"},
        "intrinsic never duplicates self");

  ControlContext bounded;
  for (int index = 0; index < 31; ++index) {
    bounded.define("L" + std::to_string(index), index ? "L" + std::to_string(index - 1) : "VBox");
  }
  check(take(bounded.resolve("L30")).lineage.size() == 32, "exact bound includes natural Column");
  auto source = std::make_shared<ControlSourceOrigin>();
  source->sourceIdentity = "boundary.scene";
  bounded.define("TooDeep", "L30", {.origin = source});
  const auto result = bounded.validate();
  fails(result, Code::INHERITANCE_LIMIT);
  REQUIRE(result.error().diagnostic);
  CHECK(result.error().diagnostic->origin == source);
}

TEST_CASE("Linear settings share canonical padding storage and preserve sparse winning origins") {
  auto inherited = std::make_shared<ControlSourceOrigin>();
  inherited->sourceIdentity = "defaults.scene";
  auto local = std::make_shared<ControlSourceOrigin>();
  local->sourceIdentity = "instance.scene";
  ControlSettings defaults;
  defaults.grid.paddingLeft = 4;
  defaults.grid.paddingTop = 2;
  defaults.linear.mainAlign = LayoutAlign::END;
  defaults.linear.crossAlign = LayoutAlign::CENTER;
  defaults.linear.overflow = LayoutOverflow::CLIP;
  defaults.origin = inherited;
  ControlSettings instance;
  instance.grid.paddingLeft = 0;
  instance.origin = local;
  ControlContext context;

  context.defaults("VBox", defaults).define("Padded", "VBox");

  const auto resolved = take(context.resolve("Padded", instance));
  CHECK(resolved.materializedLeaves == 5);
  CHECK(resolved.settings.grid.paddingLeft == 0);
  CHECK(resolved.settings.grid.paddingTop == 2);
  CHECK(resolved.settings.linear.mainAlign == LayoutAlign::END);
  const auto left = std::ranges::find(resolved.settings.origins, "padding.left", &ControlPropertyOrigin::path);
  const auto top = std::ranges::find(resolved.settings.origins, "padding.top", &ControlPropertyOrigin::path);
  REQUIRE(left != resolved.settings.origins.end());
  REQUIRE(top != resolved.settings.origins.end());
  CHECK(left->source == local);
  CHECK(top->source == inherited);

  for (const auto type : {"VBox", "HBox"}) {
    const auto schema = controlPropertySchema(type);

    CHECK(std::ranges::count(schema, "padding.left", &ControlPropertyDescriptor::path) == 1);
    CHECK(std::ranges::count(schema, "mainAlign", &ControlPropertyDescriptor::path) == 1);
    CHECK(std::ranges::count(schema, "columns", &ControlPropertyDescriptor::path) == 0);
    CHECK(std::ranges::count(schema, "limits.maxCells", &ControlPropertyDescriptor::path) == 0);
    take(ControlContext{}.resolve(type, defaults));
  }

  const auto gridSchema = ui::detail::ControlContextAccess::gridSchema();

  CHECK(std::ranges::count(gridSchema, "padding.left", &ControlPropertyDescriptor::path) == 1);
  CHECK(std::ranges::count(gridSchema, "mainAlign", &ControlPropertyDescriptor::path) == 0);
  fails(ControlContext{}.resolve("Button", defaults), Code::UNSUPPORTED_PROPERTY);

  ControlSettings invalid;
  invalid.grid.paddingBottom = -1;
  invalid.origin = local;
  const auto failure = ControlContext{}.resolve("HBox", invalid);

  fails(failure, Code::INVALID_VALUE);
  REQUIRE(failure.error().diagnostic);
  CHECK(failure.error().diagnostic->path == "HBox.padding.bottom");
  CHECK(failure.error().diagnostic->origin == local);
  invalid = {};
  invalid.linear.mainAlign = static_cast<LayoutAlign>(-1);
  fails(ControlContext{}.resolve("VBox", invalid), Code::INVALID_VALUE);
  invalid = {};
  invalid.linear.overflow = static_cast<LayoutOverflow>(1);
  fails(ControlContext{}.resolve("HBox", invalid), Code::INVALID_VALUE);
}

TEST_CASE("Text limit leaves above bit 31 retain zero precedence and consume the real leaf budget") {
  ControlSettings defaults;
  defaults.limits.maxInputBytes = 8;
  defaults.limits.maxClusterBytes = 4;
  defaults.limits.maxGraphemes = 3;
  defaults.limits.maxCells = 6;
  ControlSettings definition;
  definition.limits.maxCells = 4;
  ControlSettings instance;
  instance.limits.maxGraphemes = 0;
  auto source = std::make_shared<ControlSourceOrigin>();
  source->sourceIdentity = "limit.scene";
  instance.origin = source;
  ControlContext context;

  context.defaults("LineEdit", defaults).define("Bounded", "LineEdit", definition);

  const auto resolved = take(context.resolve("Bounded", instance));
  CHECK(resolved.materializedLeaves == 4);
  CHECK(resolved.settings.limits.maxInputBytes == 8);
  CHECK(resolved.settings.limits.maxClusterBytes == 4);
  CHECK(resolved.settings.limits.maxGraphemes == 0);
  CHECK(resolved.settings.limits.maxCells == 4);
  const auto origin = std::ranges::find(resolved.settings.origins, "limits.maxGraphemes", &ControlPropertyOrigin::path);
  REQUIRE(origin != resolved.settings.origins.end());
  CHECK(origin->source == source);
  CHECK(ui::detail::controlSettingsPresence(resolved.settings) == 0);
  CHECK(ui::detail::controlSettingsPresenceSet(resolved.settings).count() == 4);

  ControlContext exact({.maxMaterializedLeaves = 4});
  exact.defaults("LineEdit", defaults);
  take(exact.resolve("LineEdit", instance));
  ControlContext tooSmall({.maxMaterializedLeaves = 3});
  tooSmall.defaults("LineEdit", defaults);
  fails(tooSmall.validate(), Code::MATERIALIZED_LEAF_LIMIT);
  ControlContext shallow({.maxStructDepth = 1});
  fails(shallow.resolve("LineEdit", instance), Code::STRUCT_DEPTH_LIMIT);
  fails(ControlContext{}.resolve("VBox", instance), Code::UNSUPPORTED_PROPERTY);
}

TEST_CASE("ordinary Overlay schema is scoped and its two tail leaves share canonical padding origins and budgets") {
  ControlContext original;
  original.defaults("Overlay", {.grid = {.paddingLeft = 1}, .horizontalAlign = LayoutAlign::CENTER})
    .define("Layer", "Overlay", {.verticalAlign = LayoutAlign::END});
  fails(original.validate(), Code::UNKNOWN_TYPE);
  auto effective = original;
  take(ui::detail::ControlContextAccess::installOverlaySchema(effective));
  auto origin = std::make_shared<ControlSourceOrigin>();
  origin->sourceIdentity = "overlay.scene";
  origin->byteOffset = 19;
  const auto resolved = take(effective.resolve("Layer", {.origin = origin, .horizontalAlign = LayoutAlign::START}));
  CHECK(resolved.factoryKind == ui::detail::ControlFactoryKind::OVERLAY_FALLBACK);
  CHECK(resolved.settings.grid.paddingLeft == 1);
  CHECK(resolved.settings.horizontalAlign == LayoutAlign::START);
  CHECK(resolved.settings.verticalAlign == LayoutAlign::END);
  CHECK(resolved.materializedLeaves == 3);
  const auto found = std::ranges::find(resolved.settings.origins, "horizontalAlign", &ControlPropertyOrigin::path);
  REQUIRE(found != resolved.settings.origins.end());
  CHECK(found->source == origin);
  CHECK(ui::detail::controlSettingsPresence(
          ControlSettings{.horizontalAlign = LayoutAlign::START, .verticalAlign = LayoutAlign::END}) == 0);
  fails(effective.resolve("Overlay", {.grid = {.paddingTop = -1}}), Code::INVALID_VALUE);
  fails(effective.resolve("Overlay", {.horizontalAlign = static_cast<LayoutAlign>(9)}), Code::INVALID_VALUE);
  fails(effective.resolve("Overlay", {.verticalAlign = static_cast<LayoutAlign>(9)}), Code::INVALID_VALUE);
  fails(effective.resolve("Overlay", {.horizontal = ScrollBarPolicy::AUTO}), Code::UNSUPPORTED_PROPERTY);
  fails(effective.resolve("Overlay", {.gap = 1}), Code::UNSUPPORTED_PROPERTY);
  for (const std::size_t budget : {1U, 2U}) {
    ControlContext bounded(ControlContextLimits{.maxMaterializedLeaves = budget});
    take(ui::detail::ControlContextAccess::installOverlaySchema(bounded));
    const auto value =
      bounded.resolve("Overlay", {.horizontalAlign = LayoutAlign::START, .verticalAlign = LayoutAlign::STRETCH});
    if (budget == 1) {
      fails(value, Code::MATERIALIZED_LEAF_LIMIT);
    } else {
      CHECK(take(value).materializedLeaves == 2);
    }
  }
  ControlContext caller;
  caller.define("Overlay", "Label");
  take(ui::detail::ControlContextAccess::installOverlaySchema(caller));
  CHECK(take(caller.resolve("Overlay")).base == "Label");
}

TEST_CASE("Scroll scalar schemas retain full logical widths sparse frame leaves and scoped names") {
  static_assert(std::same_as<decltype(ControlSettings::keyStep), std::optional<std::uint64_t>>);
  static_assert(std::same_as<decltype(ControlSettings::pageOverlap), std::optional<std::uint64_t>>);
  ControlContext original;
  original.defaults("ScrollArea", {.wheelStep = 2.5, .frame = {.enabled = true}})
    .define("Viewport", "ScrollArea", {.pageOverlap = 0, .focusable = false});
  fails(original.validate(), Code::UNKNOWN_TYPE);
  auto context = original;
  take(ui::detail::ControlContextAccess::installScrollSchema(context, "ScrollArea"));
  take(ui::detail::ControlContextAccess::installScrollSchema(context, "ScrollBar"));
  auto source = std::make_shared<ControlSourceOrigin>();
  source->sourceIdentity = "scroll.scene";
  source->byteOffset = 42;
  const auto resolved = take(context.resolve(
    "Viewport",
    {.origin = source, .keyStep = std::numeric_limits<std::uint64_t>::max(), .frame = {.maxPaintCells = 9}}));
  CHECK(resolved.factoryKind == ui::detail::ControlFactoryKind::SCROLL_FALLBACK);
  CHECK(resolved.settings.keyStep == std::numeric_limits<std::uint64_t>::max());
  CHECK(resolved.settings.pageOverlap == 0);
  CHECK(resolved.settings.focusable == false);
  CHECK(resolved.settings.frame.enabled == true);
  CHECK(resolved.materializedLeaves == 6);
  CHECK(ui::detail::controlSettingsPresence(resolved.settings) == 0);
  const auto found = std::ranges::find(resolved.settings.origins, "frame.maxPaintCells", &ControlPropertyOrigin::path);
  REQUIRE(found != resolved.settings.origins.end());
  CHECK(found->source == source);
  for (const double invalid :
       {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    fails(context.resolve("ScrollBar", {.wheelStep = invalid}), Code::INVALID_VALUE);
  }
  fails(context.resolve("ScrollArea", {.keyStep = 0}), Code::INVALID_VALUE);
  fails(context.resolve("ScrollArea", {.minimumThumb = 0}), Code::INVALID_VALUE);
  fails(context.resolve("ScrollArea", {.horizontal = static_cast<ScrollBarPolicy>(9)}), Code::INVALID_VALUE);
  fails(context.resolve("ScrollArea", {.placement = static_cast<ScrollPlacement>(9)}), Code::INVALID_VALUE);
  fails(context.resolve("ScrollArea", {.frame = {.maxPaintCells = 0}}), Code::INVALID_VALUE);
  fails(context.resolve("ScrollBar", {.axis = static_cast<ScrollAxis>(9)}), Code::INVALID_VALUE);
  take(context.resolve("ScrollBar", {.frame = {.enabled = false}}));
  fails(context.resolve("ScrollArea", {.axis = ScrollAxis::HORIZONTAL}), Code::UNSUPPORTED_PROPERTY);
  take(context.resolve("LineEdit", {.readOnly = false}));
  ControlContext named;
  named.define("ScrollArea", "Label");
  take(ui::detail::ControlContextAccess::installScrollSchema(named, "ScrollArea"));
  CHECK(take(named.resolve("ScrollArea")).base == "Label");
  for (const std::size_t budget : {2U, 3U}) {
    ControlContext bounded(ControlContextLimits{.maxMaterializedLeaves = budget});
    take(ui::detail::ControlContextAccess::installScrollSchema(bounded, "ScrollArea"));
    const auto frame = bounded.resolve(
      "ScrollArea", {.frame = {.enabled = true, .glyphs = PrimitiveGlyphs::ASCII, .maxPaintCells = 16}});
    if (budget == 2) {
      fails(frame, Code::MATERIALIZED_LEAF_LIMIT);
    } else {
      CHECK(take(frame).materializedLeaves == 3);
    }
  }
}

TEST_CASE("Primitive scalar schemas remain scoped and retain finite values zero and winning origins") {
  ControlContext original;
  original.defaults("Gauge", {.glyphs = PrimitiveGlyphs::ASCII, .maximum = 10, .preferredWidth = 0})
    .define("Progress", "Gauge", {.minimum = 2});
  fails(original.validate(), Code::UNKNOWN_TYPE);

  auto effective = original;
  take(ui::detail::ControlContextAccess::installPrimitiveSchema(effective, "Gauge"));
  take(ui::detail::ControlContextAccess::installPrimitiveSchema(effective, "Border"));
  take(effective.validate());
  auto source = std::make_shared<ControlSourceOrigin>();
  source->sourceIdentity = "primitive.scene";
  source->byteOffset = 17;
  const auto resolved = take(effective.resolve("Progress", {.origin = source, .value = 0}));

  CHECK(resolved.factoryKind == ui::detail::ControlFactoryKind::PRIMITIVE_FALLBACK);
  CHECK(resolved.settings.value == 0);
  CHECK(resolved.settings.minimum == 2);
  CHECK(resolved.settings.maximum == 10);
  CHECK(resolved.settings.preferredWidth == 0);
  CHECK(resolved.materializedLeaves == 5);
  CHECK(ui::detail::controlSettingsPresence(resolved.settings) == 0);
  const auto origin = std::ranges::find(resolved.settings.origins, "value", &ControlPropertyOrigin::path);
  REQUIRE(origin != resolved.settings.origins.end());
  CHECK(origin->source == source);
  fails(original.resolve("Gauge"), Code::UNKNOWN_TYPE);

  for (const double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}) {
    const auto failure = effective.resolve("Gauge", {.origin = source, .value = invalid});

    fails(failure, Code::INVALID_VALUE);
    REQUIRE(failure.error().diagnostic);
    CHECK(failure.error().diagnostic->path == "Gauge.value");
    CHECK(failure.error().diagnostic->origin == source);
  }

  fails(effective.resolve("Gauge", {.maxPaintCells = 0}), Code::INVALID_VALUE);
  fails(effective.resolve("Gauge", {.preferredWidth = -1}), Code::INVALID_VALUE);
  fails(effective.resolve("Border", {.glyphs = static_cast<PrimitiveGlyphs>(9)}), Code::INVALID_VALUE);
  fails(effective.resolve("Border", {.value = 0}), Code::UNSUPPORTED_PROPERTY);
  fails(effective.resolve("Button", {.chrome = static_cast<ButtonChrome>(9)}), Code::INVALID_VALUE);
  fails(effective.resolve("Label", {.chrome = ButtonChrome::NONE}), Code::UNSUPPORTED_PROPERTY);
  take(effective.resolve("Button", {.chrome = ButtonChrome::NONE}));

  constexpr std::string_view SCALAR_PATHS[]{"value", "minimum", "maximum", "preferredWidth"};
  ControlContext custom;
  custom.registerType("CustomScalar", SCALAR_PATHS);
  take(custom.resolve("CustomScalar", {.value = 0.5, .minimum = -2, .maximum = 4, .preferredWidth = 0}));

  ControlContext named;
  named.define("Border", "Label");
  take(ui::detail::ControlContextAccess::installPrimitiveSchema(named, "Border"));
  const auto declared = take(named.resolve("Border"));

  CHECK(declared.base == "Label");
  CHECK(declared.factoryKind == ui::detail::ControlFactoryKind::EXISTING_BUILTIN);
}

TEST_CASE("native key-help literal schemas cross both presence words without changing legacy low32") {
  static_assert(ui::detail::CONTROL_PROPERTY_COUNT >= 69);
  ControlContext source({.maxMaterializedLeaves = 4});
  source.defaults("HelpView", {.format = {.strokeSeparator = " -> "}, .emptyText = "default empty"})
    .define("Guide", "HelpView", {.format = {.unknownMarker = "?"}});
  auto context = source;
  take(ui::detail::ControlContextAccess::installKeyHelpSchema(context, "HelpView"));
  take(ui::detail::ControlContextAccess::installKeyHelpSchema(context, "KeyHints"));
  fails(source.validate(), Code::UNKNOWN_TYPE);
  auto origin = std::make_shared<ControlSourceOrigin>();
  origin->sourceIdentity = "help.scene";
  origin->byteOffset = 23;
  const auto resolved = take(context.resolve("Guide", {.origin = origin, .initiallyOpen = false}));
  CHECK(resolved.settings.initiallyOpen == false);
  CHECK(resolved.settings.format.strokeSeparator == " -> ");
  CHECK(resolved.settings.format.unknownMarker == "?");
  CHECK(resolved.settings.emptyText == "default empty");
  CHECK(resolved.materializedLeaves == 4);
  CHECK(resolved.factoryKind == ui::detail::ControlFactoryKind::KEY_HELP_FALLBACK);
  const auto presence = ui::detail::controlSettingsPresenceSet(resolved.settings);
  CHECK(presence.test(58));
  CHECK(presence.test(60));
  CHECK(presence.test(64));
  CHECK(presence.test(65));
  CHECK(presence.count() == 4);
  CHECK(ui::detail::controlSettingsPresence(resolved.settings) == 0);
  auto lowAndHigh = resolved.settings;
  lowAndHigh.width = 0;
  CHECK(ui::detail::controlSettingsPresence(lowAndHigh) == 2);

  const auto atLimit = take(context.resolve("Guide", {.unavailableText = "fourth leaf"}));

  CHECK(atLimit.materializedLeaves == 4);
  // Guide inherits three leaves; explicit false remains a fourth leaf before the new fifth leaf.
  fails(context.resolve("Guide", {.initiallyOpen = false, .unavailableText = "fifth leaf"}),
        Code::MATERIALIZED_LEAF_LIMIT);

  auto invalid = context.resolve("Guide", {.origin = origin, .format = {.maxBytesPerBinding = 0}});
  fails(invalid, Code::INVALID_VALUE);
  REQUIRE(invalid.error().diagnostic);
  CHECK(invalid.error().diagnostic->path == "Guide.format.maxBytesPerBinding");
  CHECK(invalid.error().diagnostic->origin == origin);
  CHECK(take(context.resolve("KeyHints", {.format = {.strokeSeparator = std::string(70000, 's')}}))
          .settings.format.strokeSeparator->size() == 70000);
  fails(context.resolve("KeyHints", {.overflowMarker = ""}), Code::INVALID_VALUE);
  fails(context.resolve("KeyHints", {.emptyText = std::string(1, static_cast<char>(0xff))}), Code::INVALID_VALUE);
  fails(context.resolve("KeyHints", {.initiallyOpen = true}), Code::UNSUPPORTED_PROPERTY);
  fails(context.resolve("HelpView", {.entrySeparator = "|"}), Code::UNSUPPORTED_PROPERTY);
  const auto schema = controlPropertySchema("KeyHints");
  const auto stringLeaf = std::ranges::find(schema, "emptyText", &ControlPropertyDescriptor::path);
  REQUIRE(stringLeaf != schema.end());
  CHECK(stringLeaf->kind == ControlPropertyKind::LITERAL_STRING);
  CHECK(std::ranges::count(schema, "format.maxBytesPerBinding", &ControlPropertyDescriptor::path) == 1);
  CHECK(std::ranges::count(schema, "text", &ControlPropertyDescriptor::path) == 0);
  ControlContext caller;
  caller.define("HelpView", "Label");
  take(ui::detail::ControlContextAccess::installKeyHelpSchema(caller, "HelpView"));
  CHECK(take(caller.resolve("HelpView")).base == "Label");
}

TEST_CASE("provider-bound list table scalar schemas retain high-word leaves and native defaults") {
  static_assert(ui::detail::CONTROL_PROPERTY_COUNT >= 74);
  ControlContext original({.maxMaterializedLeaves = 5});
  original.defaults("VirtualTable", {.rowHeight = 2, .overscanRows = 4})
    .define("Prices", "VirtualTable", {.maxColumns = 3});
  auto context = original;
  take(ui::detail::ControlContextAccess::installVirtualViewSchema(context, "VirtualList"));
  take(ui::detail::ControlContextAccess::installVirtualViewSchema(context, "VirtualTable"));
  fails(original.validate(), Code::UNKNOWN_TYPE);
  auto origin = std::make_shared<ControlSourceOrigin>();
  origin->sourceIdentity = "data.scene";
  origin->byteOffset = 41;

  const auto effective = take(context.resolve(
    "Prices", {.origin = origin, .overscanRows = 0, .maxPreparedCells = 6, .maxPreparedTextBytes = 64}));

  CHECK(effective.settings.rowHeight == 2);
  CHECK(effective.settings.overscanRows == 0);
  CHECK(effective.settings.maxColumns == 3);
  CHECK(effective.materializedLeaves == 5);
  const auto presence = ui::detail::controlSettingsPresenceSet(effective.settings);

  for (std::size_t index = 69; index < 74; ++index) {
    CHECK(presence.test(index));
  }
  CHECK(presence.count() == 5);
  CHECK(ui::detail::controlSettingsPresence(effective.settings) == 0);
  auto options = ui::detail::controlVirtualTableOptions(effective.settings, {});

  CHECK(options.row_height == 2);
  CHECK(options.overscan_rows == 0);
  CHECK(options.max_columns == 3);
  CHECK(options.max_prepared_cells == 6);
  CHECK(options.max_prepared_text_bytes == 64);
  VirtualListOptions receiving;

  receiving.row_height = 7;
  receiving.overscan_rows = 9;
  receiving.normal.bold = true;
  const auto sparse = ui::detail::controlVirtualListOptions({.overscanRows = 0}, receiving);

  CHECK(sparse.row_height == 7);
  CHECK(sparse.overscan_rows == 0);
  CHECK(sparse.normal.bold);
  const auto defaults = ui::detail::controlVirtualTableOptions({}, {});

  CHECK(defaults.row_height == 1);
  CHECK(defaults.overscan_rows == 2);
  CHECK(defaults.max_columns == 64);
  CHECK(defaults.max_prepared_cells == 16384);
  CHECK(defaults.max_prepared_text_bytes == 1048576);
  fails(context.resolve("VirtualList", {.rowHeight = 0}), Code::INVALID_VALUE);
  fails(context.resolve("VirtualList", {.rowHeight = -1}), Code::INVALID_VALUE);
  fails(context.resolve("VirtualList", {.maxColumns = 1}), Code::UNSUPPORTED_PROPERTY);
  fails(context.resolve("VirtualTable", {.maxColumns = 0}), Code::INVALID_VALUE);
  fails(context.resolve("VirtualTable", {.maxPreparedCells = 0}), Code::INVALID_VALUE);
  fails(context.resolve("VirtualTable", {.maxPreparedTextBytes = 0}), Code::INVALID_VALUE);
  fails(context.resolve("Prices", {.width = 1, .maxPreparedCells = 6, .maxPreparedTextBytes = 64}),
        Code::MATERIALIZED_LEAF_LIMIT);
  const auto invalid = context.resolve("VirtualTable", {.origin = origin, .rowHeight = 0});

  REQUIRE_FALSE(invalid);
  REQUIRE(invalid.error().diagnostic);
  CHECK(invalid.error().diagnostic->path == "VirtualTable.rowHeight");
  CHECK(invalid.error().diagnostic->origin == origin);
  ControlContext caller;

  caller.define("VirtualList", "Label");
  take(ui::detail::ControlContextAccess::installVirtualViewSchema(caller, "VirtualList"));
  CHECK(take(caller.resolve("VirtualList")).base == "Label");
}

TEST_CASE("chart scalar schema preserves optional native scale and complete high-word accounting") {
  static_assert(ui::detail::CONTROL_PROPERTY_COUNT >= 80);
  const auto checkedOptions = [](const ControlSettings &settings, ChartOptions options) {
    const auto result = ui::detail::controlChartOptions(settings, options);

    check(result.has_value(), "expected checked chart options");

    return *result;
  };
  ControlContext original({.maxMaterializedLeaves = 6});

  original
    .defaults("Chart", {.mode = ChartMode::CANDLES,
                        .chartGlyphSet = ChartGlyphSet::BOX_DRAWING,
                        .candleGap = 1,
                        .yRange = {.enabled = true, .minimum = 90, .maximum = 120}})
    .define("Prices", "Chart");
  auto context = original;

  take(ui::detail::ControlContextAccess::installVirtualViewSchema(context, "Chart"));
  fails(original.validate(), Code::UNKNOWN_TYPE);
  const auto effective = take(context.resolve("Prices"));
  const auto presence = ui::detail::controlSettingsPresenceSet(effective.settings);

  CHECK(effective.materializedLeaves == 6);
  CHECK(presence.count() == 6);
  CHECK(ui::detail::controlSettingsPresence(effective.settings) == 0);
  for (std::size_t index = 74; index < 80; ++index) {
    CHECK(presence.test(index));
  }
  ControlContext shortBudget({.maxMaterializedLeaves = 5});

  take(ui::detail::ControlContextAccess::installVirtualViewSchema(shortBudget, "Chart"));
  fails(shortBudget.resolve("Chart", effective.settings), Code::MATERIALIZED_LEAF_LIMIT);
  auto origin = std::make_shared<ControlSourceOrigin>();

  origin->sourceIdentity = "prices.scene";
  origin->byteOffset = 19;

  const auto reversed = context.resolve("Prices", {.origin = origin, .yRange = {.maximum = 80}});

  fails(reversed, Code::INVALID_VALUE);
  REQUIRE(reversed.error().diagnostic);
  CHECK(reversed.error().diagnostic->path == "Prices.yRange.maximum");
  CHECK(reversed.error().diagnostic->origin == origin);
  const auto cleared = take(context.resolve("Prices", {.yRange = {.enabled = false, .minimum = 200, .maximum = -200}}));
  ChartOptions actual;

  actual.y_range = ChartRange{10, 20};
  actual.line.bold = true;

  const auto disabled = checkedOptions(cleared.settings, actual);

  CHECK_FALSE(disabled.y_range);
  CHECK(disabled.line.bold);
  const auto unchanged = checkedOptions({}, actual);

  REQUIRE(unchanged.y_range);
  CHECK(unchanged.y_range->minimum == 10);
  CHECK(unchanged.y_range->maximum == 20);
  const auto created = checkedOptions({.yRange = {.enabled = true}}, {});

  REQUIRE(created.y_range);
  CHECK(created.y_range->minimum == 0);
  CHECK(created.y_range->maximum == 0);
  CHECK_FALSE(ui::detail::controlChartOptions({.yRange = {.minimum = 1}}, {}));
  const auto boundOnly = checkedOptions({.yRange = {.minimum = 15}}, actual);

  CHECK(boundOnly.y_range->minimum == 15);
  CHECK(boundOnly.y_range->maximum == 20);
  fails(context.resolve("Chart", {.mode = static_cast<ChartMode>(99)}), Code::INVALID_VALUE);
  fails(context.resolve("Chart", {.chartGlyphSet = static_cast<ChartGlyphSet>(99)}), Code::INVALID_VALUE);
  fails(context.resolve("Chart", {.candleGap = -1}), Code::INVALID_VALUE);
  fails(context.resolve("Label", {.candleGap = 0}), Code::UNSUPPORTED_PROPERTY);
  for (const double invalid : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    fails(context.resolve("Chart", {.yRange = {.enabled = false, .minimum = invalid}}), Code::INVALID_VALUE);
    fails(context.resolve("Chart", {.yRange = {.maximum = invalid}}), Code::INVALID_VALUE);
  }
  ControlContext caller;

  caller.define("Chart", "Label");
  take(ui::detail::ControlContextAccess::installVirtualViewSchema(caller, "Chart"));
  CHECK(take(caller.resolve("Chart")).base == "Label");
}

TEST_CASE("data-state sparse labels preserve native services and full-width interval without expression resolution") {
  static_assert(ui::detail::CONTROL_PROPERTY_COUNT >= 90);
  constexpr SchedulerTime INTERVAL = (SchedulerTime{1} << 32) + 100;
  ControlContext original({.maxMaterializedLeaves = 10});

  original
    .defaults("DataStateView", {.showStatus = true,
                                .statusRows = 2,
                                .contentPolicy = DataContentPolicy::REPLACE,
                                .spinnerInterval = INTERVAL,
                                .labels = {.loading = i18n("missing"),
                                           .empty = text("empty"),
                                           .error = text("error"),
                                           .offline = text("offline"),
                                           .refreshing = text("refreshing"),
                                           .retry = i18n("unused")}})
    .define("Result", "DataStateView");
  auto context = original;

  take(ui::detail::ControlContextAccess::installDataStateSchema(context));
  fails(original.validate(), Code::UNKNOWN_TYPE);
  const auto resolved = take(context.resolve("Result", {.showStatus = false}));
  const auto presence = ui::detail::controlSettingsPresenceSet(resolved.settings);

  CHECK(resolved.materializedLeaves == 10);
  CHECK(presence.count() == 10);
  CHECK(ui::detail::controlSettingsPresence(resolved.settings) == 0);
  for (std::size_t index = 80; index < 90; ++index) {
    CHECK(presence.test(index));
  }
  CHECK(resolved.settings.spinnerInterval == INTERVAL);
  ControlContext shortBudget({.maxMaterializedLeaves = 9});

  take(ui::detail::ControlContextAccess::installDataStateSchema(shortBudget));
  fails(shortBudget.resolve("DataStateView", resolved.settings), Code::MATERIALIZED_LEAF_LIMIT);
  DataStateConfig actual;

  actual.statusRows = 7;
  actual.spinnerInterval = 333;
  actual.labels.loading = TextValue("actual loading");
  actual.policy.width.preferred = 19;
  actual.resolver = [](std::string_view) -> std::expected<std::string, TextResourceError> {
    throw std::runtime_error("unused resolver");
  };
  actual.retry = []() -> std::expected<void, TreeError> {
    return {};
  };
  auto effective =
    ui::detail::controlDataStateConfig({.spinnerInterval = INTERVAL, .labels = {.error = text("patched")}}, actual);

  CHECK(effective.statusRows == 7);
  CHECK(effective.spinnerInterval == INTERVAL);
  CHECK(effective.policy.width.preferred == 19);
  CHECK(bool(effective.resolver));
  CHECK(bool(effective.retry));
  CHECK(effective.labels.loading.resolve({})->bytes() == "actual loading");
  CHECK(effective.labels.error.resolve({})->bytes() == "patched");
  const auto defaults = ui::detail::controlDataStateConfig({}, {});

  CHECK(defaults.showStatus);
  CHECK(defaults.statusRows == 1);
  CHECK(defaults.contentPolicy == DataContentPolicy::KEEP_BODY);
  CHECK(defaults.spinnerInterval == 100);
  fails(context.resolve("DataStateView", {.showStatus = false, .statusRows = 0}), Code::INVALID_VALUE);
  fails(context.resolve("DataStateView", {.spinnerInterval = 0}), Code::INVALID_VALUE);
  fails(context.resolve("DataStateView", {.contentPolicy = static_cast<DataContentPolicy>(99)}), Code::INVALID_VALUE);
  fails(context.resolve("Label", {.showStatus = false}), Code::UNSUPPORTED_PROPERTY);
  take(context.resolve(
    "DataStateView", {.showStatus = false, .labels = {.loading = TextValue(std::string(1, static_cast<char>(0xff)))}}));
}

TEST_CASE("host and dialog sparse options retain actual native domains caps and canonical provenance") {
  static_assert(ui::detail::CONTROL_PROPERTY_COUNT >= 101);
  static_assert(!std::derived_from<Dialog, SizedWidget>);
  ControlContext context;

  take(ui::detail::ControlContextAccess::installDialogSchema(context, "OverlayHost"));
  take(ui::detail::ControlContextAccess::installDialogSchema(context, "Dialog"));
  context
    .defaults("Dialog", {.title = text("Default"),
                         .dialog = {.mode = DialogMode::NON_MODAL,
                                    .placement = DialogPlacement::ABOVE_START,
                                    .maximumPanelSize = {.width = 19, .height = 7},
                                    .viewportMargin = 2,
                                    .anchorGap = 3,
                                    .closeOnEscape = false,
                                    .frame = {.glyphs = PrimitiveGlyphs::ASCII, .maxPaintCells = 900}}})
    .define("Popup", "Dialog", {.dialog = {.maximumPanelSize = {.height = 6}}});
  const auto resolved = take(context.resolve("Popup"));
  const auto presence = ui::detail::controlSettingsPresenceSet(resolved.settings);

  CHECK(resolved.materializedLeaves == 10);
  CHECK(presence.count() == 10);
  CHECK(ui::detail::controlSettingsPresence(resolved.settings) == 0);
  for (std::size_t index = 91; index < 101; ++index) {
    CHECK(presence.test(index));
  }
  CHECK(take(context.resolve("OverlayHost", {.maxLayers = 2})).materializedLeaves == 1);
  DialogOptions actual;

  actual.mode = DialogMode::MODAL;
  actual.maximumPanelSize = {30, 11};
  actual.viewportMargin = 4;
  actual.frame.maxPaintCells = 700;
  const auto sparse = ui::detail::controlDialogOptions(
    {.dialog = {.maximumPanelSize = {.height = 5}, .frame = {.glyphs = PrimitiveGlyphs::ASCII}}}, actual);

  CHECK(sparse.maximumPanelSize == CellSize{30, 5});
  CHECK(sparse.mode == DialogMode::MODAL);
  CHECK(sparse.viewportMargin == 4);
  CHECK(sparse.frame.maxPaintCells == 700);
  CHECK(sparse.frame.glyphs == PrimitiveGlyphs::ASCII);
  const auto effective = ui::detail::controlDialogOptions(resolved.settings, actual);

  CHECK(effective.mode == DialogMode::NON_MODAL);
  CHECK(effective.placement == DialogPlacement::ABOVE_START);
  CHECK(effective.maximumPanelSize == CellSize{19, 6});
  CHECK(effective.viewportMargin == 2);
  CHECK(effective.anchorGap == 3);
  CHECK_FALSE(effective.closeOnEscape);
  CHECK(effective.frame.maxPaintCells == 900);
  CHECK(ui::detail::controlOverlayHostOptions({}, {.maxLayers = 5}).maxLayers == 5);
  CHECK(ui::detail::controlOverlayHostOptions({.maxLayers = 2}, {.maxLayers = 5}).maxLayers == 2);
  fails(context.resolve("OverlayHost", {.maxLayers = 0}), Code::INVALID_VALUE);
  fails(context.resolve("Dialog", {.dialog = {.maximumPanelSize = {.width = 0}}}), Code::INVALID_VALUE);
  fails(context.resolve("Dialog", {.dialog = {.maximumPanelSize = {.height = -1}}}), Code::INVALID_VALUE);
  fails(context.resolve("Dialog", {.dialog = {.viewportMargin = -1}}), Code::INVALID_VALUE);
  fails(context.resolve("Dialog", {.dialog = {.anchorGap = -1}}), Code::INVALID_VALUE);
  fails(context.resolve("Dialog", {.dialog = {.frame = {.maxPaintCells = 0}}}), Code::INVALID_VALUE);
  fails(context.resolve("Dialog", {.dialog = {.mode = static_cast<DialogMode>(99)}}), Code::INVALID_VALUE);
  fails(context.resolve("Dialog", {.dialog = {.placement = static_cast<DialogPlacement>(99)}}), Code::INVALID_VALUE);
  fails(context.resolve("Dialog", {.mode = ChartMode::LINE}), Code::UNSUPPORTED_PROPERTY);
  fails(context.resolve("Dialog", {.placement = ScrollPlacement::OVERLAY}), Code::UNSUPPORTED_PROPERTY);
  fails(context.resolve("Dialog", {.width = 5}), Code::UNSUPPORTED_PROPERTY);
  fails(context.resolve("OverlayHost", {.horizontalAlign = LayoutAlign::CENTER}), Code::UNSUPPORTED_PROPERTY);
  ControlContext shallow({.maxStructDepth = 2});

  take(ui::detail::ControlContextAccess::installDialogSchema(shallow, "Dialog"));
  fails(shallow.resolve("Dialog", {.dialog = {.frame = {.glyphs = PrimitiveGlyphs::ASCII}}}), Code::STRUCT_DEPTH_LIMIT);
  ControlContext caller;

  caller.define("Dialog", "Label");
  take(ui::detail::ControlContextAccess::installDialogSchema(caller, "Dialog"));
  CHECK(take(caller.resolve("Dialog", {.text = text("caller")})).base == "Label");
}

TEST_CASE("native MessageBox sparse schema keeps actual defaults and optional choice semantics") {
  ControlContext context;

  take(ui::detail::ControlContextAccess::installMessageSchema(context));
  const auto native = ui::detail::controlMessageOptions({}, {}, 0);

  CHECK(native.textLimits.max_input_bytes == 16384);
  CHECK(native.textLimits.max_cluster_bytes == 4096);
  CHECK(native.textLimits.max_graphemes == 16384);
  CHECK(native.textLimits.max_cells == 32768);
  CHECK(native.maxBodyLines == 4096);
  CHECK_FALSE(native.defaultChoice);
  CHECK_FALSE(native.allowDestructiveDefault);
  CHECK(native.dialog.mode == DialogMode::MODAL);
  CHECK(native.dialog.closeOnEscape);
  ControlSettings all;

  all.body = text("body");
  all.message.labels = {.acknowledge = text("ok"),
                        .confirm = text("yes"),
                        .save = text("write"),
                        .discard = text("drop"),
                        .cancel = text("back")};
  all.message.defaultChoice = {.enabled = false, .value = MessageChoice::DISCARD};
  all.message.allowDestructiveDefault = false;
  all.message.textLimits = {.maxInputBytes = 40, .maxClusterBytes = 12, .maxGraphemes = 20, .maxCells = 30};
  all.message.maxBodyLines = 4;
  const auto resolved = take(context.resolve("MessageBox", all));
  const auto presence = ui::detail::controlSettingsPresenceSet(resolved.settings);

  CHECK(resolved.materializedLeaves == 14);
  CHECK(presence.count() == 14);
  CHECK(ui::detail::controlSettingsPresence(resolved.settings) == 0);
  for (std::size_t index = 101; index < 115; ++index) {
    CHECK(presence.test(index));
  }
  MessageOptions actual;

  actual.defaultChoice = MessageChoice::SAVE;
  actual.maxBodyLines = 9;
  actual.dialog.maximumPanelSize = {23, 8};
  auto disabled = ui::detail::controlMessageOptions(resolved.settings, actual, 2);

  CHECK_FALSE(disabled.defaultChoice);
  CHECK(disabled.maxBodyLines == 4);
  CHECK(disabled.dialog.maximumPanelSize == CellSize{23, 8});
  CHECK(ui::detail::controlMessageOptions({.message = {.defaultChoice = {.enabled = true}}}, actual, 2).defaultChoice ==
        MessageChoice::SAVE);
  CHECK(ui::detail::controlMessageOptions({.message = {.defaultChoice = {.enabled = true}}}, {}, 0).defaultChoice ==
        MessageChoice::ACKNOWLEDGE);
  CHECK(ui::detail::controlMessageOptions({.message = {.defaultChoice = {.enabled = true}}}, {}, 1).defaultChoice ==
        MessageChoice::CANCEL);
  CHECK(ui::detail::controlMessageOptions({.message = {.defaultChoice = {.value = MessageChoice::DISCARD}}}, actual, 2)
          .defaultChoice == MessageChoice::DISCARD);
  fails(context.resolve("MessageBox",
                        {.message = {.defaultChoice = {.enabled = false, .value = static_cast<MessageChoice>(99)}}}),
        Code::INVALID_VALUE);
  fails(context.resolve("MessageBox", {.message = {.textLimits = {.maxInputBytes = 0}}}), Code::INVALID_VALUE);
  fails(context.resolve("MessageBox", {.message = {.textLimits = {.maxClusterBytes = 0}}}), Code::INVALID_VALUE);
  fails(context.resolve("MessageBox", {.message = {.textLimits = {.maxGraphemes = 0}}}), Code::INVALID_VALUE);
  fails(context.resolve("MessageBox", {.message = {.textLimits = {.maxCells = 0}}}), Code::INVALID_VALUE);
  fails(context.resolve("MessageBox", {.message = {.maxBodyLines = 0}}), Code::INVALID_VALUE);
  fails(context.resolve("MessageBox", {.dialog = {.mode = DialogMode::NON_MODAL}}), Code::INVALID_VALUE);
  fails(context.resolve("MessageBox", {.dialog = {.closeOnEscape = false}}), Code::INVALID_VALUE);
  fails(context.resolve("MessageBox", {.width = 3}), Code::UNSUPPORTED_PROPERTY);
  take(ui::detail::ControlContextAccess::installDialogSchema(context, "Dialog"));
  fails(context.resolve("Dialog", {.body = text("message-only")}), Code::UNSUPPORTED_PROPERTY);
  ControlContext shallow({.maxStructDepth = 2});

  take(ui::detail::ControlContextAccess::installMessageSchema(shallow));
  fails(shallow.resolve("MessageBox", {.message = {.labels = {.cancel = text("back")}}}), Code::STRUCT_DEPTH_LIMIT);
}

TEST_CASE("Notifications sparse construction preserves captured options and third-word presence") {
  ControlContext context;

  take(ui::detail::ControlContextAccess::installNotificationSchema(context));
  const auto native = ui::detail::controlNotificationOptions({}, {});

  CHECK(native.capacity == 32);
  CHECK(native.maxVisible == 3);
  CHECK(native.placement == ToastPlacement::BOTTOM_END);
  CHECK(native.maximumWidth == 48);
  CHECK(native.margin == 1);
  CHECK(native.gap == 1);
  CHECK(native.overflow == ToastOverflowPolicy::DROP_OLDEST_PENDING);
  CHECK_FALSE(native.refreshDeduplicated);
  CHECK_FALSE(native.reducedMotion);
  CHECK(native.textLimits.max_input_bytes == 16384);
  CHECK(native.textLimits.max_cluster_bytes == 4096);
  CHECK(native.textLimits.max_graphemes == 16384);
  CHECK(native.textLimits.max_cells == 32768);
  CHECK(native.actionLabelBytes == 256);
  CHECK(native.dedupKeyBytes == 128);
  CHECK(native.maxPaintCells == 16384);
  const ControlSettings all{
    .notification = {.capacity = 4,
                     .maxVisible = 2,
                     .placement = ToastPlacement::TOP_START,
                     .maximumWidth = 12,
                     .margin = 0,
                     .gap = 0,
                     .overflow = ToastOverflowPolicy::REJECT_NEW,
                     .refreshDeduplicated = false,
                     .reducedMotion = true,
                     .textLimits = {.maxInputBytes = 64, .maxClusterBytes = 12, .maxGraphemes = 32, .maxCells = 32},
                     .actionLabelBytes = 4,
                     .dedupKeyBytes = 4,
                     .maxPaintCells = 512}};
  const auto resolved = take(context.resolve("Notifications", all));
  const auto presence = ui::detail::controlSettingsPresenceSet(resolved.settings);

  CHECK(resolved.materializedLeaves == 16);
  CHECK(presence.count() == 16);
  CHECK(presence.test(127));
  CHECK(presence.test(128));
  CHECK(presence.test(130));
  CHECK(ui::detail::controlSettingsPresence(resolved.settings) == 0);
  for (std::size_t index = 115; index < 131; ++index) {
    CHECK(presence.test(index));
  }
  NotificationOptions actual;

  actual.capacity = 9;
  actual.maxVisible = 5;
  actual.margin = 3;
  actual.textLimits = {95, 17, 75, 80};
  actual.refreshDeduplicated = true;
  actual.textResolver = [](std::string_view) -> std::expected<std::string, TextResourceError> {
    return "own";
  };
  const auto sparse = ui::detail::controlNotificationOptions(
    {.notification = {.maxVisible = 1, .refreshDeduplicated = false, .textLimits = {.maxCells = 31}}}, actual);

  CHECK(sparse.capacity == 9);
  CHECK(sparse.maxVisible == 1);
  CHECK(sparse.margin == 3);
  CHECK_FALSE(sparse.refreshDeduplicated);
  CHECK(sparse.textLimits.max_input_bytes == 95);
  CHECK(sparse.textLimits.max_cells == 31);
  REQUIRE(sparse.textResolver);
  CHECK(sparse.textResolver("k").value() == "own");
  context.defaults("Notifications", {.notification = {.capacity = 4}});
  context.define("Inbox", "Notifications", {.notification = {.maxVisible = 2}});
  const auto inherited = take(context.resolve("Inbox", {.notification = {.gap = 0}}));

  CHECK(inherited.settings.notification.capacity == 4);
  CHECK(inherited.settings.notification.maxVisible == 2);
  CHECK(inherited.settings.notification.gap == 0);
  fails(context.resolve("Inbox", {.notification = {.maxVisible = 5}}), Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.capacity = 0}}), Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.maxVisible = 0}}), Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.maximumWidth = 0}}), Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.margin = -1}}), Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.gap = -1}}), Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.placement = static_cast<ToastPlacement>(99)}}),
        Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.overflow = static_cast<ToastOverflowPolicy>(99)}}),
        Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.textLimits = {.maxInputBytes = 0}}}), Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.textLimits = {.maxClusterBytes = 0}}}),
        Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.textLimits = {.maxGraphemes = 0}}}), Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.textLimits = {.maxCells = 0}}}), Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.actionLabelBytes = 0}}), Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.dedupKeyBytes = 0}}), Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.notification = {.maxPaintCells = 0}}), Code::INVALID_VALUE);
  fails(context.resolve("Notifications", {.text = text("not body")}), Code::UNSUPPORTED_PROPERTY);
  fails(context.resolve("Notifications", {.title = text("not title")}), Code::UNSUPPORTED_PROPERTY);
  ControlContext caller;

  caller.define("Overlay", "Label");
  take(ui::detail::ControlContextAccess::installNotificationSchema(caller));
  CHECK(take(caller.resolve("Overlay", {.text = text("caller")})).base == "Label");
  CHECK(take(caller.resolve("Notifications", {.notification = {.capacity = 4}})).base == "Notifications");
}

TEST_CASE("ThemeSelector view literals preserve actual options and optional shortcut semantics") {
  ControlContext controls;

  take(ui::detail::ControlContextAccess::installThemeSelectorSchema(controls));
  CHECK(controlPropertySchema("ThemeSelector").size() == 25);
  const auto native = ui::detail::controlThemeSelectorOptions({}, {});

  CHECK(native.title.resolve(WidthPolicy{}).value().bytes() == "Themes");
  CHECK(native.apply.resolve(WidthPolicy{}).value().bytes() == "Apply");
  CHECK(native.cancel.resolve(WidthPolicy{}).value().bytes() == "Cancel");
  CHECK(native.closeKeepingPreview.resolve(WidthPolicy{}).value().bytes() == "Close (keep preview)");
  CHECK(native.emptyStatus.resolve(WidthPolicy{}).value().bytes() == "Choose a theme to preview");
  CHECK(native.errorStatus.resolve(WidthPolicy{}).value().bytes() ==
        "Theme action failed; retry or close keeping preview");
  CHECK(native.conflictStatus.resolve(WidthPolicy{}).value().bytes() ==
        "Theme changed elsewhere; preview again or close keeping preview");
  CHECK(native.textLimits.max_input_bytes == 4096);
  CHECK(native.textLimits.max_cluster_bytes == 4096);
  CHECK(native.textLimits.max_graphemes == 4096);
  CHECK(native.textLimits.max_cells == 8192);
  CHECK(native.openingShortcut == "Ctrl+K, Ctrl+T");
  const ControlSettings all{
    .themeSelector = {.labels = {.apply = text("Use"),
                                 .cancel = text("Back"),
                                 .closeKeepingPreview = text("Keep"),
                                 .emptyStatus = text("Pick"),
                                 .errorStatus = text("Error"),
                                 .conflictStatus = text("Conflict")},
                      .textLimits = {.maxInputBytes = 128, .maxClusterBytes = 64, .maxGraphemes = 64, .maxCells = 96},
                      .openingShortcut = {.enabled = false, .value = "Ctrl+J"}}};
  const auto resolved = take(controls.resolve("ThemeSelector", all));
  const auto presence = ui::detail::controlSettingsPresenceSet(resolved.settings);

  CHECK(resolved.materializedLeaves == 12);
  CHECK(presence.count() == 12);
  for (std::size_t index = 131; index < 143; ++index) {
    CHECK(presence.test(index));
  }
  CHECK(ui::detail::controlSettingsPresence(resolved.settings) == 0);
  ThemeSelectorOptions captured;

  captured.textLimits.max_cells = 72;
  captured.openingShortcut = "Ctrl+Q";
  captured.textResolver = [](std::string_view) -> std::expected<std::string, TextResourceError> {
    return "Own";
  };
  const auto sparse =
    ui::detail::controlThemeSelectorOptions({.themeSelector = {.labels = {.cancel = text("Back")}}}, captured);

  CHECK(sparse.textLimits.max_cells == 72);
  CHECK(sparse.openingShortcut == "Ctrl+Q");
  CHECK(sparse.textResolver("k").value() == "Own");
  CHECK_FALSE(ui::detail::controlThemeSelectorOptions(all, captured).openingShortcut);
  CHECK(ui::detail::controlThemeSelectorOptions({.themeSelector = {.openingShortcut = {.value = "Ctrl+J"}}}, captured)
          .openingShortcut == "Ctrl+J");
  CHECK(ui::detail::controlThemeSelectorOptions({.themeSelector = {.openingShortcut = {.enabled = true}}}, captured)
          .openingShortcut == "Ctrl+Q");
  captured.openingShortcut.reset();
  CHECK(ui::detail::controlThemeSelectorOptions({.themeSelector = {.openingShortcut = {.enabled = true}}}, captured)
          .openingShortcut == "Ctrl+K, Ctrl+T");
  controls.defaults("ThemeSelector", {.themeSelector = {.textLimits = {.maxInputBytes = 64}}});
  controls.define("Picker", "ThemeSelector", {.themeSelector = {.labels = {.apply = text("Use")}}});
  CHECK(take(controls.resolve("Picker")).settings.themeSelector.textLimits.maxInputBytes == 64);
  fails(controls.resolve("ThemeSelector",
                         {.themeSelector = {.openingShortcut = {.enabled = false, .value = "Ctrl+DefinitelyNotAKey"}}}),
        Code::INVALID_VALUE);
  fails(controls.resolve("ThemeSelector", {.themeSelector = {.textLimits = {.maxInputBytes = 0}}}),
        Code::INVALID_VALUE);
  fails(controls.resolve("ThemeSelector", {.themeSelector = {.textLimits = {.maxClusterBytes = 0}}}),
        Code::INVALID_VALUE);
  fails(controls.resolve("ThemeSelector", {.themeSelector = {.textLimits = {.maxGraphemes = 0}}}), Code::INVALID_VALUE);
  fails(controls.resolve("ThemeSelector", {.themeSelector = {.textLimits = {.maxCells = 0}}}), Code::INVALID_VALUE);
  fails(controls.resolve("ThemeSelector", {.dialog = {.mode = DialogMode::NON_MODAL}}), Code::INVALID_VALUE);
  fails(controls.resolve("ThemeSelector", {.body = text("not body")}), Code::UNSUPPORTED_PROPERTY);
  fails(controls.resolve("ThemeSelector", {.width = 4}), Code::UNSUPPORTED_PROPERTY);
}

TEST_CASE("ThemeSelectorCatalog keeps finite constructor options and optional activation semantics") {
  ControlContext controls;
  take(ui::detail::ControlContextAccess::installThemeCatalogSchema(controls));
  CHECK(controlPropertySchema("ThemeSelector").size() == 25);
  CHECK(controlPropertySchema("ThemeSelectorCatalog").size() == 41);
  const ControlSettings all{
    .themeCatalog = {
      .maxChoices = 3,
      .maxIdBytes = 12,
      .maxLabelBytes = 24,
      .maxDescriptionBytes = 48,
      .maxTextBytes = 256,
      .textLimits = {.maxInputBytes = 64, .maxClusterBytes = 32, .maxGraphemes = 16, .maxCells = 24},
      .initialId = {.enabled = false, .value = "red"},
      .layers = {.enabled = false, .maxLayers = 4, .maxRules = 8, .maxDeclarations = 16, .maxOwnedBytes = 512}}};
  const auto resolved = take(controls.resolve("ThemeSelectorCatalog", all));
  CHECK(resolved.materializedLeaves == 16);
  const auto presence = ui::detail::controlSettingsPresenceSet(resolved.settings);
  for (std::size_t index = 143; index < 159; ++index) {
    CHECK(presence.test(index));
  }
  CHECK(ui::detail::controlSettingsPresence(all) == 0);
  ThemeSelectionOptions options;
  std::optional<std::string> initial;
  REQUIRE(ui::detail::controlThemeCatalogOptions({}, options, initial));
  CHECK(options.maxChoices == 64);
  CHECK(options.maxIdBytes == 128);
  CHECK(options.maxLabelBytes == 512);
  CHECK(options.maxDescriptionBytes == 2048);
  CHECK(options.maxTextBytes == 1048576);
  CHECK(options.textLimits.max_input_bytes == 2048);
  CHECK(options.textLimits.max_cluster_bytes == 4096);
  CHECK(options.textLimits.max_graphemes == 16384);
  CHECK(options.textLimits.max_cells == 32768);
  CHECK_FALSE(options.layers);
  options.textResolver = [](std::string_view) -> std::expected<std::string, TextResourceError> {
    return "Own";
  };
  options.layers.emplace();
  initial = "captured";
  REQUIRE(ui::detail::controlThemeCatalogOptions(all, options, initial));
  CHECK(options.maxChoices == 3);
  CHECK(options.textLimits.max_cells == 24);
  CHECK(options.textResolver("k").value() == "Own");
  CHECK_FALSE(initial);
  CHECK_FALSE(options.layers);
  REQUIRE(ui::detail::controlThemeCatalogOptions(
    {.themeCatalog = {.initialId = {.value = "red"}, .layers = {.maxRules = 7}}}, options, initial));
  CHECK(initial == "red");
  REQUIRE(options.layers);
  CHECK(options.layers->maxLayers == 16);
  CHECK(options.layers->maxRules == 7);
  REQUIRE(ui::detail::controlThemeCatalogOptions({.themeCatalog = {.initialId = {.enabled = true}}}, options, initial));
  CHECK(initial == "red");
  initial.reset();
  const auto missingInitial =
    ui::detail::controlThemeCatalogOptions({.themeCatalog = {.initialId = {.enabled = true}}}, options, initial);
  REQUIRE_FALSE(missingInitial);
  CHECK(missingInitial.error() == TreeError::INVALID_ARGUMENT);
  fails(controls.resolve("ThemeSelector", all), Code::UNSUPPORTED_PROPERTY);
  fails(controls.resolve("ThemeSelectorCatalog", {.themeCatalog = {.maxChoices = 0}}), Code::INVALID_VALUE);
  fails(controls.resolve("ThemeSelectorCatalog", {.themeCatalog = {.layers = {.enabled = false, .maxLayers = 1}}}),
        Code::INVALID_VALUE);
  fails(controls.resolve("ThemeSelectorCatalog", {.themeCatalog = {.textLimits = {.maxCells = 0}}}),
        Code::INVALID_VALUE);
}

TEST_CASE("actual catalog fixed ancestry retains sparse precedence origins and user inheritance limits") {
  auto viewOrigin = std::make_shared<ControlSourceOrigin>();
  auto catalogOrigin = std::make_shared<ControlSourceOrigin>();
  auto instanceOrigin = std::make_shared<ControlSourceOrigin>();
  viewOrigin->sourceIdentity = "view.cfg";
  catalogOrigin->sourceIdentity = "catalog.cfg";
  instanceOrigin->sourceIdentity = "instance.cfg";
  ControlContext controls({.maxInheritance = 1});
  take(ui::detail::ControlContextAccess::installThemeCatalogSchema(controls));
  controls.defaults("ThemeSelector", {.origin = viewOrigin, .themeSelector = {.labels = {.apply = text("A")}}});
  controls.defaults(
    "ThemeSelectorCatalog",
    {.origin = catalogOrigin, .themeSelector = {.labels = {.apply = text("B")}}, .themeCatalog = {.maxChoices = 2}});
  controls.define("Picker", "ThemeSelector");
  auto actual = controls;
  take(ui::detail::ControlContextAccess::installThemeCatalogSchema(actual, true));
  const auto oldView = take(controls.resolve("Picker"));
  const auto inherited = take(actual.resolve("Picker"));
  CHECK(oldView.settings.themeSelector.labels.apply->resolve({}).value().bytes() == "A");
  CHECK(inherited.settings.themeSelector.labels.apply->resolve({}).value().bytes() == "B");
  CHECK(inherited.settings.themeCatalog.maxChoices == 2);
  CHECK(inherited.lineage == oldView.lineage);
  const auto source =
    std::ranges::find(inherited.settings.origins, "themeSelector.labels.apply", &ControlPropertyOrigin::path);
  REQUIRE(source != inherited.settings.origins.end());
  CHECK(source->source == catalogOrigin);
  const auto explicitValue =
    take(actual.resolve("Picker", {.origin = instanceOrigin, .themeSelector = {.labels = {.apply = text("C")}}}));
  CHECK(explicitValue.settings.themeSelector.labels.apply->resolve({}).value().bytes() == "C");
  const auto explicitOrigin =
    std::ranges::find(explicitValue.settings.origins, "themeSelector.labels.apply", &ControlPropertyOrigin::path);
  REQUIRE(explicitOrigin != explicitValue.settings.origins.end());
  CHECK(explicitOrigin->source == instanceOrigin);
  CHECK(explicitValue.materializedLeaves == 2);
  controls.define("TooDeep", "Picker");
  fails(controls.validate(), Code::INHERITANCE_LIMIT);
}

TEST_CASE("Shared frame, Spinner and scrollbar schemas append stable leaves within four presence words") {
  static_assert(ui::detail::CONTROL_PROPERTY_COUNT == 231);
  static_assert((ui::detail::CONTROL_PRESENCE_BITS + 63) / 64 == 4);
  ControlSettings appended;
  appended.spinnerPreset = SpinnerPreset::LINE;
  appended.scrollbar.pattern = ScrollbarPattern::ASCII;
  const auto appendedPresence = ui::detail::controlSettingsPresenceSet(appended);

  CHECK(appendedPresence.count() == 2);
  CHECK(appendedPresence.test(193));
  CHECK(appendedPresence.test(204));

  ControlSettings patch;
  patch.frame.enabled = true;
  patch.frame.glyphs = PrimitiveGlyphs::ASCII;
  patch.frame.maxPaintCells = 1024;
  patch.frame.pattern = FramePattern::ROUNDED;
  patch.frame.sides = {false, true, false, true};
  patch.frame.title = text("Title");
  patch.frame.titleAlignment = FrameTitleAlignment::CENTER;
  patch.frame.maxTitleBytes = 100;
  patch.frame.customGlyphs = {true, "+", "+", "+", "+", "-", "|"};
  const auto presence = ui::detail::controlSettingsPresenceSet(patch);

  CHECK(presence.count() == 18);
  CHECK(presence.test(53));
  CHECK(presence.test(54));
  CHECK(presence.test(55));

  for (std::size_t index = 165; index < 180; ++index) {
    CHECK(presence.test(index));
  }

  CHECK(ui::detail::controlSettingsPresence(patch) == 0);

  for (const auto type :
       {"Button",   "Label",       "CheckBox",     "ThreeState",    "LineEdit",    "Password",      "VBox",
        "HBox",     "Border",      "Gauge",        "ScrollArea",    "ScrollBar",   "Overlay",       "HelpView",
        "KeyHints", "VirtualList", "VirtualTable", "DataStateView", "OverlayHost", "Notifications", "Grid"}) {
    const auto schema =
      type == std::string_view("Grid") ? ui::detail::ControlContextAccess::gridSchema() : controlPropertySchema(type);
    REQUIRE_FALSE(schema.empty());

    for (const auto &descriptor : schema) {
      CHECK(std::ranges::count(schema, descriptor.path, &ControlPropertyDescriptor::path) == 1);
    }

    CHECK(std::ranges::count(schema, "frame.enabled", &ControlPropertyDescriptor::path) == 1);
    CHECK(std::ranges::count(schema, "frame.customGlyphs.vertical", &ControlPropertyDescriptor::path) == 1);
    ControlContext context;
    std::vector<std::string_view> paths;

    for (const auto &descriptor : schema) {
      paths.push_back(descriptor.path);
    }

    context.registerType("FrameReceiver", paths);
    CHECK(take(context.resolve("FrameReceiver", patch)).materializedLeaves == 18);
  }

  for (const auto type : {"Dialog", "MessageBox", "ThemeSelector", "ThemeSelectorCatalog", "Chart"}) {
    CHECK(std::ranges::count(controlPropertySchema(type), "frame.enabled", &ControlPropertyDescriptor::path) == 0);
  }

  const auto schema = controlPropertySchema("Button");
  const auto pattern = std::ranges::find(schema, "frame.pattern", &ControlPropertyDescriptor::path);
  REQUIRE(pattern != schema.end());
  CHECK(pattern->enumNames.size() == 6);
  CHECK(std::ranges::find(pattern->enumNames, "PLAIN") != pattern->enumNames.end());
  CHECK(ui::detail::ControlContextAccess::canonicalProperty("placeHolder") == "placeholder");
  for (const auto value : {FramePattern::SINGLE, FramePattern::PLAIN, FramePattern::ROUNDED, FramePattern::DOUBLE,
                           FramePattern::HEAVY, FramePattern::ASCII}) {
    take(ControlContext{}.resolve("Button", {.frame = {.pattern = value}}));
  }

  fails(ControlContext{}.resolve("Button", {.frame = {.pattern = static_cast<FramePattern>(99)}}), Code::INVALID_VALUE);
  fails(ControlContext{}.resolve("Button", {.frame = {.titleAlignment = static_cast<FrameTitleAlignment>(99)}}),
        Code::INVALID_VALUE);
  fails(ControlContext{}.resolve("Button", {.frame = {.maxTitleBytes = 0}}), Code::INVALID_VALUE);
  fails(ControlContext{}.resolve("Button", {.frame = {.customGlyphs = {.topLeft = std::string(65, '+')}}}),
        Code::INVALID_VALUE);
}

TEST_CASE("Frame leaves merge independently with winning origins") {
  ControlSettings defaults;
  defaults.frame.enabled = true;
  defaults.frame.pattern = FramePattern::DOUBLE;
  defaults.frame.sides.left = true;
  defaults.frame.title = text("Default");
  defaults.frame.customGlyphs.enabled = false;
  ControlContext context;
  context.defaults("Button", defaults)
    .define("Framed", "Button", {.frame = {.sides = {.top = false}, .maxTitleBytes = 20}});
  auto source = std::make_shared<ControlSourceOrigin>();
  source->sourceIdentity = "frame.scene";
  const auto resolved = take(context.resolve(
    "Framed", {.origin = source, .frame = {.pattern = FramePattern::ASCII, .title = text("Instance")}}));

  CHECK(resolved.materializedLeaves == 7);
  CHECK(resolved.settings.frame.pattern == FramePattern::ASCII);
  CHECK(resolved.settings.frame.sides.left == true);
  CHECK(resolved.settings.frame.sides.top == false);
  const auto origin = std::ranges::find(resolved.settings.origins, "frame.title", &ControlPropertyOrigin::path);
  REQUIRE(origin != resolved.settings.origins.end());
  CHECK(origin->source == source);
  const auto options = take(ui::detail::controlFrameOptions(resolved.settings, {}, {}));
  REQUIRE(options);
  CHECK(options->title == "Instance");
  CHECK(options->maxTitleBytes == 20);
}

TEST_CASE("Frame helper preserves bases and checks enablement custom sources and titles") {
  using ui::detail::controlFrameOptions;
  CHECK_FALSE(take(controlFrameOptions({}, {}, {})));
  auto options = take(controlFrameOptions({.frame = {.enabled = true}}, {}, {}));
  REQUIRE(options);
  CHECK(options->maxTitleBytes == 4096);
  CHECK(options->pattern == FramePattern::SINGLE);
  CHECK_FALSE(take(controlFrameOptions({.frame = {.enabled = false}}, options, {})));
  CHECK_FALSE(controlFrameOptions({.frame = {.pattern = FramePattern::ROUNDED}}, {}, {}));
  CHECK_FALSE(controlFrameOptions({.frame = {.enabled = false, .title = text("Title")}}, options, {}));
  options->title = "Base";
  options->sides.right = false;
  options = take(controlFrameOptions({.frame = {.glyphs = PrimitiveGlyphs::ASCII}}, options, {}));
  REQUIRE(options);
  CHECK(options->title == "Base");
  CHECK_FALSE(options->sides.right);
  CHECK(options->border.glyphs == PrimitiveGlyphs::ASCII);
  CHECK_FALSE(controlFrameOptions({.frame = {.maxTitleBytes = 0}}, options, {}));
  CHECK_FALSE(controlFrameOptions({.frame = {.maxTitleBytes = 2}}, options, {}));
  CHECK_FALSE(controlFrameOptions({.frame = {.customGlyphs = {.enabled = true}}}, options, {}));
  CHECK_FALSE(controlFrameOptions({.frame = {.customGlyphs = {.topLeft = "+"}}}, options, {}));
  options = take(controlFrameOptions({.frame = {.customGlyphs = {true, "+", "+", "+", "+", "-", "|"}}}, options, {}));
  REQUIRE(options);
  REQUIRE(options->customGlyphs);
  CHECK(options->customGlyphs->horizontal == "-");
  options = take(controlFrameOptions({.frame = {.customGlyphs = {.horizontal = "="}}}, options, {}));
  CHECK(options->customGlyphs->horizontal == "=");
  CHECK(options->customGlyphs->topLeft == "+");
  CHECK_FALSE(controlFrameOptions({.frame = {.customGlyphs = {.enabled = false, .vertical = "|"}}}, options, {}));
  CHECK_FALSE(controlFrameOptions({.frame = {.customGlyphs = {.vertical = std::string(65, '|')}}}, options, {}));
  options = take(controlFrameOptions({.frame = {.customGlyphs = {.enabled = false}}}, options, {}));
  CHECK_FALSE(options->customGlyphs);
  const auto invalid = controlFrameOptions({.frame = {.title = text(std::string_view("\xff", 1))}}, options, {});
  REQUIRE_FALSE(invalid);
  CHECK(std::get<TreeError>(invalid.error().cause) == TreeError::INVALID_ARGUMENT);
  REQUIRE(invalid.error().text);
  CHECK(invalid.error().text->kind == TextErrorKind::INVALID_UTF8);
  REQUIRE(invalid.error().sceneResource);
  REQUIRE(invalid.error().sceneResource->propertyPath);
  CHECK(*invalid.error().sceneResource->propertyPath == "frame.title");
  const auto oversized = controlFrameOptions({.frame = {.title = text("12345"), .maxTitleBytes = 4}}, options, {});
  REQUIRE_FALSE(oversized);
  REQUIRE(oversized.error().text);
  CHECK(oversized.error().text->kind == TextErrorKind::INPUT_LIMIT);
}

TEST_CASE("title decoration sparse settings preserve ends and share checked enum byte limits") {
  using ui::detail::controlFrameOptions;
  FrameOptions base;
  base.title = "Title";
  base.titleDecoration = TitleDecoration::ROUND;
  base.customTitleDecoration = CustomTitleDecoration{"[ ", " ]"};
  auto inherited = take(controlFrameOptions({}, base, {}));
  REQUIRE(inherited);
  CHECK(inherited->titleDecoration == TitleDecoration::ROUND);
  REQUIRE(inherited->customTitleDecoration);
  CHECK(inherited->customTitleDecoration->prefix == "[ ");
  ControlSettings patch;
  patch.frame.titleDecoration = TitleDecoration::SQUARE;
  patch.frame.customTitleDecoration.suffix = "!";
  auto merged = take(controlFrameOptions(patch, inherited, {}));
  REQUIRE(merged);
  CHECK(merged->customTitleDecoration->prefix == "[ ");
  CHECK(merged->customTitleDecoration->suffix == "!");
  patch.frame.customTitleDecoration = {.enabled = false};
  CHECK_FALSE(take(controlFrameOptions(patch, inherited, {}))->customTitleDecoration);
  patch.frame.customTitleDecoration = {.enabled = true, .prefix = "", .suffix = ""};
  CHECK_FALSE(controlFrameOptions(patch, {}, {}));
  patch.frame.enabled = true;
  CHECK(ControlContext{}.resolve("Button", patch));
  CHECK(take(controlFrameOptions(patch, {}, {}))->customTitleDecoration->prefix.empty());
  patch.frame.titleDecoration = static_cast<TitleDecoration>(99);
  CHECK_FALSE(ControlContext{}.resolve("Button", patch));
  patch.frame.titleDecoration = TitleDecoration::SPACED;
  patch.frame.customTitleDecoration.prefix = std::string(65, 'a');
  CHECK_FALSE(ControlContext{}.resolve("Button", patch));
}

TEST_CASE("scrollbar scalar vocabulary appends old identities and merges only supplied leaves") {
  ui::ControlContext controls;
  take(ui::detail::ControlContextAccess::installScrollSchema(controls, "ScrollArea"));
  ControlSettings base;
  base.scrollbar.pattern = ScrollbarPattern::FRAME;
  base.scrollbar.ends = true;
  base.scrollbar.horizontal.track = "H";
  base.scrollbar.vertical.thumb = "Y";
  controls.defaults("ScrollArea", base);
  ControlSettings patch;
  patch.scrollbar.ends = false;
  patch.scrollbar.horizontal.thumb = "X";
  const auto result = take(controls.resolve("ScrollArea", patch));
  CHECK(result.settings.scrollbar.pattern == ScrollbarPattern::FRAME);
  CHECK(result.settings.scrollbar.ends == false);
  CHECK(result.settings.scrollbar.horizontal.track == "H");
  CHECK(result.settings.scrollbar.horizontal.thumb == "X");
  CHECK(result.settings.scrollbar.vertical.thumb == "Y");
  CHECK_FALSE(controls.resolve("Button", patch));
  patch.scrollbar.pattern = static_cast<ScrollbarPattern>(99);
  CHECK_FALSE(controls.resolve("ScrollArea", patch));
  patch.scrollbar.pattern = ScrollbarPattern::ASCII;
  patch.scrollbar.maxGlyphBytes = 0;
  CHECK_FALSE(controls.resolve("ScrollArea", patch));
}

TEST_CASE("choice strategy is a typed sparse leaf restricted to the choice family") {
  ControlContext context;
  const auto choice = take(context.resolve("ComboBox", {.searchStrategy = SearchStrategy::PREFIX}));
  CHECK(choice.settings.searchStrategy == SearchStrategy::PREFIX);
  CHECK(choice.materializedLeaves == 1);
  const auto drop = take(context.resolve("DropDown", {.searchStrategy = SearchStrategy::FUZZY_PREFIX}));
  CHECK(drop.settings.searchStrategy == SearchStrategy::FUZZY_PREFIX);
  fails(context.resolve("LineEdit", {.searchStrategy = SearchStrategy::PREFIX}), Code::UNSUPPORTED_PROPERTY);
  fails(context.resolve("ComboBox", {.searchStrategy = static_cast<SearchStrategy>(99)}), Code::INVALID_VALUE);
}
