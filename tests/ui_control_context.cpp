// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui_control_context.hpp>

#include <algorithm>
#include <array>
#include <source_location>
#include <thread>
#include <type_traits>

namespace {
using namespace portals;
using namespace portals::ui;
using Code = ControlContextErrorCode;

void check(bool condition, std::string_view message, std::source_location location = std::source_location::current()) {
  INFO("Caller line: ", location.line());
  REQUIRE_MESSAGE(condition, message);
}

template <class T> T take(std::expected<T, ControlContextError> result) {
  check(result.has_value(), "expected success");

  if constexpr (!std::is_void_v<T>) {
    return *result;
  }
}

template <class T> void fails(std::expected<T, ControlContextError> result, Code code) {
  check(!result && result.error().code == code, "expected exact typed failure");
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
