// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/control_context.hpp>
#include <portals/style.hpp>

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace portals::ui {
namespace {
using Code = ControlContextErrorCode;
using Kind = ControlPropertyKind;
using Mask = std::uint32_t;
constexpr std::size_t GRID_PROPERTY_BEGIN = 17;
constexpr std::array<std::string_view, 3> STATE_NAMES{"UNCHECKED", "CHECKED", "MIXED"};
constexpr std::array<std::string_view, 2> CYCLE_NAMES{"TWO_STATE", "THREE_STATE"};
constexpr std::array<std::string_view, 3> MODE_NAMES{"IMMEDIATE", "ON_COMPLETION", "EXPLICIT"};
constexpr std::array<std::string_view, 2> CHANGE_NAMES{"RECORD", "RESET_BASELINE"};
constexpr std::array<std::string_view, 3> DISPLAY_NAMES{"NORMAL", "PASSWORD", "NO_ECHO"};
constexpr std::array<std::string_view, 2> FLOW_NAMES{"ROW_MAJOR", "COLUMN_MAJOR"};
constexpr std::array<ControlPropertyDescriptor, 30> PROPERTIES{
  {{"text", Kind::TEXT, 1, {}},
   {"width", Kind::SIGNED_INTEGER, 1, {}},
   {"height", Kind::SIGNED_INTEGER, 1, {}},
   {"flex", Kind::UNSIGNED_INTEGER, 1, {}},
   {"gap", Kind::SIGNED_INTEGER, 1, {}},
   {"state", Kind::ENUMERATION, 1, STATE_NAMES},
   {"cycle", Kind::ENUMERATION, 1, CYCLE_NAMES},
   {"editMode", Kind::ENUMERATION, 1, MODE_NAMES},
   {"readOnly", Kind::BOOLEAN, 1, {}},
   {"programmaticChange", Kind::ENUMERATION, 1, CHANGE_NAMES},
   {"displayMode", Kind::ENUMERATION, 1, DISPLAY_NAMES},
   {"placeholder", Kind::TEXT, 1, {}},
   {"history.enabled", Kind::BOOLEAN, 2, {}},
   {"history.maxOperations", Kind::UNSIGNED_INTEGER, 2, {}},
   {"history.maxBytes", Kind::UNSIGNED_INTEGER, 2, {}},
   {"history.groupTyping", Kind::BOOLEAN, 2, {}},
   {"history.groupDeletion", Kind::BOOLEAN, 2, {}},
   {"columns", Kind::UNSIGNED_INTEGER, 1, {}},
   {"rows", Kind::UNSIGNED_INTEGER, 1, {}},
   {"flow", Kind::ENUMERATION, 1, FLOW_NAMES},
   {"columnGap", Kind::SIGNED_INTEGER, 1, {}},
   {"rowGap", Kind::SIGNED_INTEGER, 1, {}},
   {"padding.left", Kind::SIGNED_INTEGER, 2, {}},
   {"padding.top", Kind::SIGNED_INTEGER, 2, {}},
   {"padding.right", Kind::SIGNED_INTEGER, 2, {}},
   {"padding.bottom", Kind::SIGNED_INTEGER, 2, {}},
   {"maxTracksPerAxis", Kind::UNSIGNED_INTEGER, 1, {}},
   {"maxPlacements", Kind::UNSIGNED_INTEGER, 1, {}},
   {"maxPlacementWork", Kind::UNSIGNED_INTEGER, 1, {}},
   {"scroll", Kind::BOOLEAN, 1, {}}}};
constexpr Mask SIZE_MASK = (1U << 1) | (1U << 2) | (1U << 3);
constexpr Mask TEXT_MASK = 1U;
constexpr Mask CHECK_MASK = SIZE_MASK | TEXT_MASK | (1U << 5) | (1U << 6);
constexpr Mask LAYOUT_MASK = SIZE_MASK | (1U << 4);
constexpr Mask EDIT_MASK = SIZE_MASK | TEXT_MASK | (((1U << 17) - 1) & ~((1U << 7) - 1));
constexpr Mask ALL_MASK = (1U << PROPERTIES.size()) - 1;
constexpr Mask GRID_MASK = SIZE_MASK | (ALL_MASK & ~((1U << GRID_PROPERTY_BEGIN) - 1));
constexpr std::size_t BUILTIN_COUNT = 7;
constexpr auto SIZE_SCHEMA = std::to_array({PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[4]});
constexpr auto TEXT_SCHEMA = std::to_array({PROPERTIES[0], PROPERTIES[1], PROPERTIES[2], PROPERTIES[3]});
constexpr auto CHECK_SCHEMA =
  std::to_array({PROPERTIES[0], PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[5], PROPERTIES[6]});
constexpr auto EDIT_SCHEMA = std::to_array(
  {PROPERTIES[0], PROPERTIES[1], PROPERTIES[2], PROPERTIES[3], PROPERTIES[7], PROPERTIES[8], PROPERTIES[9],
   PROPERTIES[10], PROPERTIES[11], PROPERTIES[12], PROPERTIES[13], PROPERTIES[14], PROPERTIES[15], PROPERTIES[16]});

bool builtin(std::string_view name) noexcept {
  return name == "VBox" || name == "HBox" || name == "Label" || name == "Button" || name == "CheckBox" ||
         name == "LineEdit" || name == "ThreeState";
}

Mask builtinMask(std::string_view name) noexcept {
  if (name == "VBox" || name == "HBox") {
    return LAYOUT_MASK;
  }

  if (name == "CheckBox" || name == "ThreeState") {
    return CHECK_MASK;
  }

  if (name == "LineEdit") {
    return EDIT_MASK;
  }

  return TEXT_MASK | SIZE_MASK;
}

bool identifier(std::string_view name) noexcept {
  const auto first = [](unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
  };

  if (name.empty() || !first(static_cast<unsigned char>(name.front()))) {
    return false;
  }

  return std::ranges::all_of(name.substr(1), [&](unsigned char ch) {
    return first(ch) || (ch >= '0' && ch <= '9');
  });
}

std::size_t propertyIndex(std::string_view path) noexcept {
  for (std::size_t index = 0; index < PROPERTIES.size(); ++index) {
    if (PROPERTIES[index].path == path) {
      return index;
    }
  }

  return PROPERTIES.size();
}

std::shared_ptr<const ControlSourceOrigin> sourceFor(const ControlSettings &settings, std::string_view path) noexcept {
  for (const auto &entry : settings.origins) {
    if (entry.path == path) {
      return entry.source;
    }
  }

  return settings.origin;
}

ControlContextError error(Code code, std::string_view type, std::string_view path = {},
                          std::shared_ptr<const ControlSourceOrigin> origin = {}) {
  auto detail = std::make_shared<ControlContextDiagnostic>();
  detail->path = type;

  if (!path.empty()) {
    detail->path += '.';
    detail->path += path;
  }

  detail->origin = std::move(origin);

  return {code, std::move(detail)};
}

ControlContextError exceptionError() noexcept {
  try {
    throw;
  } catch (const std::bad_alloc &) {
    return {Code::NO_MEMORY, {}};
  } catch (const std::length_error &) {
    return {Code::NO_MEMORY, {}};
  } catch (...) {
    return {Code::CONFIGURATION_FAILED, {}};
  }
}

// This finite list is the complete scalar vocabulary; there is no field reflection.
#define PORTALS_CONTROL_LEAVES(X)                                                                                      \
  X(text, 0)                                                                                                           \
  X(width, 1)                                                                                                          \
  X(height, 2)                                                                                                         \
  X(flex, 3)                                                                                                           \
  X(gap, 4)                                                                                                            \
  X(state, 5)                                                                                                          \
  X(cycle, 6)                                                                                                          \
  X(editMode, 7)                                                                                                       \
  X(readOnly, 8)                                                                                                       \
  X(programmaticChange, 9)                                                                                             \
  X(displayMode, 10)                                                                                                   \
  X(placeholder, 11)                                                                                                   \
  X(history.enabled, 12)                                                                                               \
  X(history.maxOperations, 13)                                                                                         \
  X(history.maxBytes, 14)                                                                                              \
  X(history.groupTyping, 15)                                                                                           \
  X(history.groupDeletion, 16)                                                                                         \
  X(grid.columns, 17)                                                                                                  \
  X(grid.rows, 18)                                                                                                     \
  X(grid.flow, 19)                                                                                                     \
  X(grid.columnGap, 20)                                                                                                \
  X(grid.rowGap, 21)                                                                                                   \
  X(grid.paddingLeft, 22)                                                                                              \
  X(grid.paddingTop, 23)                                                                                               \
  X(grid.paddingRight, 24)                                                                                             \
  X(grid.paddingBottom, 25)                                                                                            \
  X(grid.maxTracksPerAxis, 26)                                                                                         \
  X(grid.maxPlacements, 27)                                                                                            \
  X(grid.maxPlacementWork, 28)                                                                                         \
  X(grid.scroll, 29)

template <class Visitor> void visitLeaves(const ControlSettings &settings, Visitor &&visitor) {
#define VISIT(field, index) visitor(index, settings.field);
  PORTALS_CONTROL_LEAVES(VISIT)
#undef VISIT
}

std::size_t mergedLeafCount(const ControlSettings &left, const ControlSettings &right) {
  std::array<bool, PROPERTIES.size()> present{};
  visitLeaves(left, [&](std::size_t index, const auto &value) {
    present[index] = value.has_value();
  });
  visitLeaves(right, [&](std::size_t index, const auto &value) {
    present[index] = present[index] || value.has_value();
  });

  return static_cast<std::size_t>(std::ranges::count(present, true));
}

void merge(ControlSettings &target, const ControlSettings &patch) {
  const auto assign = [&](auto &destination, const auto &value, std::size_t index) {
    if (!value) {
      return;
    }

    destination = value;
    const auto path = PROPERTIES[index].path;
    auto found = std::ranges::find(target.origins, path, &ControlPropertyOrigin::path);

    if (found == target.origins.end()) {
      ControlPropertyOrigin provenance;
      provenance.path = path;
      provenance.source = sourceFor(patch, path);
      target.origins.push_back(provenance);
    } else {
      found->source = sourceFor(patch, path);
    }
  };
#define ASSIGN(field, index) assign(target.field, patch.field, index);
  PORTALS_CONTROL_LEAVES(ASSIGN)
#undef ASSIGN
}

#undef PORTALS_CONTROL_LEAVES

template <class T> bool validValue(const T &value, std::size_t index) noexcept {
  if constexpr (std::same_as<T, std::int32_t>) {
    return value >= 0;
  } else if constexpr (std::same_as<T, std::uint32_t>) {
    return value > 0 || (index != 3 && index < GRID_PROPERTY_BEGIN);
  } else if constexpr (std::same_as<T, std::size_t>) {
    return index < GRID_PROPERTY_BEGIN || value > 0;
  } else if constexpr (std::same_as<T, GridFlow>) {
    return value == GridFlow::ROW_MAJOR || value == GridFlow::COLUMN_MAJOR;
  } else if constexpr (std::same_as<T, CheckState>) {
    return value == CheckState::UNCHECKED || value == CheckState::CHECKED || value == CheckState::MIXED;
  } else if constexpr (std::same_as<T, CheckCycle>) {
    return value == CheckCycle::TWO_STATE || value == CheckCycle::THREE_STATE;
  } else if constexpr (std::same_as<T, EditMode>) {
    return value == EditMode::IMMEDIATE || value == EditMode::ON_COMPLETION || value == EditMode::EXPLICIT;
  } else if constexpr (std::same_as<T, ProgrammaticChange>) {
    return value == ProgrammaticChange::RECORD || value == ProgrammaticChange::RESET_BASELINE;
  } else if constexpr (std::same_as<T, LineEditDisplayMode>) {
    return value == LineEditDisplayMode::NORMAL || value == LineEditDisplayMode::PASSWORD ||
           value == LineEditDisplayMode::NO_ECHO;
  } else {
    return true;
  }
}

std::expected<void, ControlContextError> validateSettings(const ControlSettings &settings, std::string_view type,
                                                          Mask mask, bool fixedCycle, ControlContextLimits limits) {
  if (settings.origins.size() > PROPERTIES.size()) {
    return std::unexpected(error(Code::DUPLICATE_PROPERTY, type));
  }

  for (std::size_t index = 0; index < settings.origins.size(); ++index) {
    const auto &origin = settings.origins[index];

    if (propertyIndex(origin.path) == PROPERTIES.size()) {
      return std::unexpected(error(Code::UNKNOWN_PROPERTY, type, origin.path, origin.source));
    }

    for (std::size_t previous = 0; previous < index; ++previous) {
      if (settings.origins[previous].path == origin.path) {
        return std::unexpected(error(Code::DUPLICATE_PROPERTY, type, origin.path, origin.source));
      }
    }
  }

  std::optional<ControlContextError> failure;
  std::size_t count = 0;
  visitLeaves(settings, [&](std::size_t index, const auto &value) {
    if (!value || failure) {
      return;
    }

    const auto &descriptor = PROPERTIES[index];
    std::optional<Code> code;

    if (!(mask & (1U << index))) {
      code = Code::UNSUPPORTED_PROPERTY;
    } else if (descriptor.depth > limits.maxStructDepth) {
      code = Code::STRUCT_DEPTH_LIMIT;
    } else if (!validValue(*value, index)) {
      code = Code::INVALID_VALUE;
    } else if (++count > limits.maxMaterializedLeaves) {
      code = Code::MATERIALIZED_LEAF_LIMIT;
    }

    if (code) {
      failure = error(*code, type, descriptor.path, sourceFor(settings, descriptor.path));
    }
  });

  if (failure) {
    return std::unexpected(*failure);
  }

  if (fixedCycle && settings.cycle && *settings.cycle != CheckCycle::THREE_STATE) {
    return std::unexpected(error(Code::FIXED_CYCLE, type, "cycle", sourceFor(settings, "cycle")));
  }

  return {};
}
} // namespace

namespace detail {
std::uint32_t controlSettingsPresence(const ControlSettings &settings) noexcept {
  Mask mask{};
  visitLeaves(settings, [&](std::size_t index, const auto &value) {
    if (value) {
      mask |= 1U << index;
    }
  });

  return mask;
}
} // namespace detail

struct ControlContext::Data {
  struct Entry {
    ~Entry() = default;

    std::string name = std::string(0, '\0');
    std::string base = std::string(0, '\0');
    ControlSettings settings;
  };

  struct Schema {
    ~Schema() = default;

    std::string name = std::string(0, '\0');
    Mask mask{};
    detail::ControlFactoryKind factoryKind{detail::ControlFactoryKind::USER};
  };

  std::optional<Schema> fallback;
  std::vector<Entry> definitions = std::vector<Entry>(0);
  std::vector<Entry> defaults = std::vector<Entry>(0);
  std::vector<Schema> schemas = std::vector<Schema>(0);
};

namespace {
struct Chain {
  ~Chain() = default;

  std::vector<std::string_view> names = std::vector<std::string_view>(0);
  std::string_view base;
  Mask mask{};
  bool fixedCycle{};
  detail::ControlFactoryKind factoryKind{detail::ControlFactoryKind::USER};
};

template <class Data> auto definition(const Data &data, std::string_view name) {
  return std::ranges::find(data.definitions, name, &Data::Entry::name);
}

template <class Data>
std::expected<Chain, ControlContextError> chainFor(const Data &data, std::string_view name, ControlContextLimits limits,
                                                   std::shared_ptr<const ControlSourceOrigin> origin) {
  Chain chain;
  auto current = name;
  auto declaration = name;
  std::string_view property;

  while (true) {
    if (std::ranges::find(chain.names, current) != chain.names.end()) {
      return std::unexpected(error(Code::INHERITANCE_CYCLE, declaration, property, origin));
    }

    if (!chain.names.empty() && chain.names.size() > limits.maxInheritance) {
      return std::unexpected(error(Code::INHERITANCE_LIMIT, declaration, property, origin));
    }

    chain.names.push_back(current);

    if (builtin(current)) {
      chain.base = current;
      chain.factoryKind = detail::ControlFactoryKind::EXISTING_BUILTIN;
      chain.mask = builtinMask(current);
      chain.fixedCycle = current == "ThreeState";

      if (chain.fixedCycle) {
        if (chain.names.size() > limits.maxInheritance) {
          return std::unexpected(error(Code::INHERITANCE_LIMIT, declaration, property, origin));
        }

        chain.names.push_back("CheckBox");
      }

      return chain;
    }

    const auto schema = std::ranges::find(data.schemas, current, &Data::Schema::name);

    if (schema != data.schemas.end()) {
      chain.base = schema->name;
      chain.mask = schema->mask;

      return chain;
    }

    const auto found = definition(data, current);

    if (found == data.definitions.end()) {
      if (data.fallback && data.fallback->name == current) {
        chain.base = data.fallback->name;
        chain.mask = data.fallback->mask;
        chain.factoryKind = data.fallback->factoryKind;
        return chain;
      }
      return std::unexpected(error(Code::UNKNOWN_TYPE, declaration, property, origin));
    }

    declaration = found->name;
    property = "base";
    origin = found->settings.origin;
    current = found->base;
  }
}

template <class Data>
std::expected<ResolvedControl, ControlContextError>
resolveData(const Data &data, std::string_view name, const ControlSettings &instance, ControlContextLimits limits,
            std::shared_ptr<const ControlSourceOrigin> origin = {}) {
  const auto chain = chainFor(data, name, limits, origin ? origin : instance.origin);

  if (!chain) {
    return std::unexpected(chain.error());
  }

  ResolvedControl result;
  result.type = name;
  result.base = chain->base;
  result.factoryKind = chain->factoryKind;

  for (std::size_t index = 1; index < chain->names.size(); ++index) {
    result.lineage.emplace_back(chain->names[index]);
  }

  const auto intrinsic = chain->base == "VBox"   ? std::string_view("Column")
                         : chain->base == "HBox" ? std::string_view("Row")
                                                 : std::string_view{};

  if (!intrinsic.empty() && intrinsic != result.type &&
      std::ranges::find(result.lineage, intrinsic) == result.lineage.end()) {
    result.lineage.emplace_back(intrinsic);
  }

  if (result.lineage.size() > limits.maxInheritance) {
    return std::unexpected(error(Code::INHERITANCE_LIMIT, name, {}, origin ? origin : instance.origin));
  }

  for (auto ancestor = chain->names.rbegin(); ancestor != chain->names.rend(); ++ancestor) {
    if (*ancestor == "ThreeState") {
      ControlSettings builtinPatch;
      builtinPatch.cycle = CheckCycle::THREE_STATE;

      if (mergedLeafCount(result.settings, builtinPatch) > limits.maxMaterializedLeaves) {
        return std::unexpected(error(Code::MATERIALIZED_LEAF_LIMIT, name));
      }

      merge(result.settings, builtinPatch);
    }

    const auto found = definition(data, *ancestor);

    if (found != data.definitions.end()) {
      const auto checked = validateSettings(found->settings, found->name, chain->mask,
                                            chain->fixedCycle && *ancestor != "CheckBox", limits);

      if (!checked) {
        return std::unexpected(checked.error());
      }

      if (mergedLeafCount(result.settings, found->settings) > limits.maxMaterializedLeaves) {
        return std::unexpected(error(Code::MATERIALIZED_LEAF_LIMIT, name));
      }

      merge(result.settings, found->settings);
    }

    const auto defaults = std::ranges::find(data.defaults, *ancestor, &Data::Entry::name);

    if (defaults != data.defaults.end()) {
      const auto checked = validateSettings(defaults->settings, defaults->name, chain->mask,
                                            chain->fixedCycle && *ancestor != "CheckBox", limits);

      if (!checked) {
        return std::unexpected(checked.error());
      }

      if (mergedLeafCount(result.settings, defaults->settings) > limits.maxMaterializedLeaves) {
        return std::unexpected(error(Code::MATERIALIZED_LEAF_LIMIT, name));
      }

      merge(result.settings, defaults->settings);
    }
  }

  const auto checked = validateSettings(instance, name, chain->mask, chain->fixedCycle, limits);

  if (!checked) {
    return std::unexpected(checked.error());
  }

  result.materializedLeaves = mergedLeafCount(result.settings, instance);

  if (result.materializedLeaves > limits.maxMaterializedLeaves) {
    return std::unexpected(error(Code::MATERIALIZED_LEAF_LIMIT, name));
  }

  merge(result.settings, instance);

  return result;
}
} // namespace

std::span<const ControlPropertyDescriptor> controlPropertySchema(std::string_view type) noexcept {
  if (type == "VBox" || type == "HBox") {
    return SIZE_SCHEMA;
  }

  if (type == "CheckBox" || type == "ThreeState") {
    return CHECK_SCHEMA;
  }

  if (type == "LineEdit") {
    return EDIT_SCHEMA;
  }

  if (type == "Label" || type == "Button") {
    return TEXT_SCHEMA;
  }

  return {};
}

namespace detail {
bool ControlContextAccess::hasDeclaredType(const ControlContext &context, std::string_view name) noexcept {
  return context.data_ && (definition(*context.data_, name) != context.data_->definitions.end() ||
                           std::ranges::find(context.data_->schemas, name, &ControlContext::Data::Schema::name) !=
                             context.data_->schemas.end());
}

bool ControlContextAccess::hasUserConfiguration(const ControlContext &context) noexcept {
  const ControlContextLimits defaults;

  return context.failure_ || context.limits_.maxDefinitions != defaults.maxDefinitions ||
         context.limits_.maxInheritance != defaults.maxInheritance ||
         context.limits_.maxStructDepth != defaults.maxStructDepth ||
         context.limits_.maxMaterializedLeaves != defaults.maxMaterializedLeaves ||
         (context.data_ &&
          (!context.data_->definitions.empty() || !context.data_->defaults.empty() || !context.data_->schemas.empty()));
}

std::span<const ControlPropertyDescriptor> ControlContextAccess::gridSchema() noexcept {
  return std::span(PROPERTIES).subspan(GRID_PROPERTY_BEGIN);
}

std::expected<void, ControlContextError> ControlContextAccess::installFallbackSchema(ControlContext &context) noexcept {
  if (context.failure_) {
    return std::unexpected(*context.failure_);
  }

  if (hasDeclaredType(context, "Grid") || (context.data_ && context.data_->fallback)) {
    return {};
  }

  try {
    auto next =
      context.data_ ? std::make_shared<ControlContext::Data>(*context.data_) : std::make_shared<ControlContext::Data>();
    ControlContext::Data::Schema schema;
    schema.name = "Grid";
    schema.mask = GRID_MASK;
    schema.factoryKind = ControlFactoryKind::DECLARATIVE_GRID_FALLBACK;
    next->fallback = schema;
    context.data_ = std::move(next);
    context.prepared_ = false;

    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}
} // namespace detail

ControlContext::ControlContext(ControlContextLimits limits) noexcept : limits_(limits) {
  if (!limits.maxDefinitions || !limits.maxInheritance || limits.maxInheritance > MAX_STYLE_BASE_TYPES ||
      !limits.maxStructDepth || !limits.maxMaterializedLeaves) {
    failure_ = ControlContextError{Code::INVALID_LIMIT, {}};
  }
}

ControlContext &ControlContext::registerType(std::string_view name,
                                             std::span<const std::string_view> properties) & noexcept {
  prepared_ = false;
  if (failure_) {
    return *this;
  }

  try {
    if (!identifier(name)) {
      failure_ = error(Code::INVALID_NAME, name);
    } else if (builtin(name)) {
      failure_ = error(Code::BUILTIN_SHADOW, name);
    } else {
      auto next = data_ ? std::make_shared<Data>(*data_) : std::make_shared<Data>();

      if (definition(*next, name) != next->definitions.end() ||
          std::ranges::find(next->schemas, name, &Data::Schema::name) != next->schemas.end()) {
        failure_ = error(Code::DUPLICATE_TYPE, name);
      } else if (next->definitions.size() + next->schemas.size() >= limits_.maxDefinitions) {
        failure_ = error(Code::DEFINITION_LIMIT, name);
      } else {
        Data::Schema schema;
        schema.name = name;

        for (const auto path : properties) {
          const auto index = propertyIndex(path);

          if (index == PROPERTIES.size()) {
            failure_ = error(Code::UNKNOWN_PROPERTY, name, path);
            break;
          }

          if (schema.mask & (1U << index)) {
            failure_ = error(Code::DUPLICATE_PROPERTY, name, path);
            break;
          }

          schema.mask |= 1U << index;
        }

        if (!failure_) {
          next->schemas.push_back(schema);
          data_ = std::move(next);
        }
      }
    }
  } catch (...) {
    failure_ = exceptionError();
  }

  return *this;
}

ControlContext &ControlContext::define(std::string_view name, std::string_view base,
                                       const ControlSettings &settings) & noexcept {
  prepared_ = false;
  if (failure_) {
    return *this;
  }

  try {
    const auto checked = validateSettings(settings, name, ALL_MASK, false, limits_);

    if (!checked) {
      failure_ = checked.error();

      return *this;
    }

    if (!identifier(name) || !identifier(base)) {
      failure_ = error(Code::INVALID_NAME, !identifier(name) ? name : base);
    } else if (builtin(name)) {
      failure_ = error(Code::BUILTIN_SHADOW, name);
    } else {
      auto next = data_ ? std::make_shared<Data>(*data_) : std::make_shared<Data>();

      if (definition(*next, name) != next->definitions.end() ||
          std::ranges::find(next->schemas, name, &Data::Schema::name) != next->schemas.end()) {
        failure_ = error(Code::DUPLICATE_TYPE, name);
      } else if (next->definitions.size() + next->schemas.size() >= limits_.maxDefinitions) {
        failure_ = error(Code::DEFINITION_LIMIT, name);
      } else {
        Data::Entry entry;
        entry.name = name;
        entry.base = base;
        entry.settings = settings;
        next->definitions.push_back(entry);
        data_ = std::move(next);
      }
    }
  } catch (...) {
    failure_ = exceptionError();
  }

  return *this;
}

ControlContext &ControlContext::defaults(std::string_view type, const ControlSettings &settings) & noexcept {
  prepared_ = false;
  if (failure_) {
    return *this;
  }

  try {
    const auto checked = validateSettings(settings, type, ALL_MASK, false, limits_);

    if (!checked) {
      failure_ = checked.error();

      return *this;
    }

    if (!identifier(type)) {
      failure_ = error(Code::INVALID_NAME, type);
    } else {
      auto next = data_ ? std::make_shared<Data>(*data_) : std::make_shared<Data>();

      if (std::ranges::find(next->defaults, type, &Data::Entry::name) != next->defaults.end()) {
        failure_ = error(Code::DUPLICATE_PROPERTY, type);
      } else if (next->defaults.size() >= BUILTIN_COUNT &&
                 next->defaults.size() - BUILTIN_COUNT >= limits_.maxDefinitions) {
        failure_ = error(Code::DEFINITION_LIMIT, type);
      } else {
        Data::Entry entry;
        entry.name = type;
        entry.settings = settings;
        next->defaults.push_back(entry);
        data_ = std::move(next);
      }
    }
  } catch (...) {
    failure_ = exceptionError();
  }

  return *this;
}

ControlContext &ControlContext::overlayDefaults(std::string_view type, const ControlSettings &settings) & noexcept {
  prepared_ = false;
  if (failure_) {
    return *this;
  }

  try {
    if (!data_ || std::ranges::find(data_->defaults, type, &Data::Entry::name) == data_->defaults.end()) {
      return defaults(type, settings);
    }

    const auto checked = validateSettings(settings, type, ALL_MASK, false, limits_);

    if (!checked) {
      failure_ = checked.error();

      return *this;
    }

    auto next = std::make_shared<Data>(*data_);
    auto &entry = *std::ranges::find(next->defaults, type, &Data::Entry::name);

    if (mergedLeafCount(entry.settings, settings) > limits_.maxMaterializedLeaves) {
      failure_ = error(Code::MATERIALIZED_LEAF_LIMIT, type);

      return *this;
    }

    merge(entry.settings, settings);
    data_ = std::move(next);
  } catch (...) {
    failure_ = exceptionError();
  }

  return *this;
}

std::expected<void, ControlContextError> ControlContext::validate() const noexcept {
  if (failure_) {
    return std::unexpected(*failure_);
  }

  if (prepared_ || !data_) {
    return {};
  }

  try {
    for (const auto &entry : data_->definitions) {
      const auto resolved = resolveData(*data_, entry.name, {}, limits_, entry.settings.origin);

      if (!resolved) {
        return std::unexpected(resolved.error());
      }
    }

    for (const auto &entry : data_->defaults) {
      const auto resolved = resolveData(*data_, entry.name, {}, limits_, entry.settings.origin);

      if (!resolved) {
        return std::unexpected(resolved.error());
      }
    }

    return {};
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

std::expected<ControlContext, ControlContextError> ControlContext::prepared() const noexcept {
  const auto checked = validate();
  if (!checked) {
    return std::unexpected(checked.error());
  }
  auto result = *this;
  result.prepared_ = true;
  return result;
}

bool ControlContext::empty() const noexcept {
  const ControlContextLimits defaults;
  return !data_ && !failure_ && limits_.maxDefinitions == defaults.maxDefinitions &&
         limits_.maxInheritance == defaults.maxInheritance && limits_.maxStructDepth == defaults.maxStructDepth &&
         limits_.maxMaterializedLeaves == defaults.maxMaterializedLeaves;
}

bool ControlContext::hasRegisteredType(std::string_view name) const noexcept {
  return data_ && std::ranges::find(data_->schemas, name, &Data::Schema::name) != data_->schemas.end();
}

bool ControlContext::matchesRegisteredType(std::string_view name,
                                           std::span<const std::string_view> properties) const noexcept {
  if (!data_) {
    return false;
  }
  const auto found = std::ranges::find(data_->schemas, name, &Data::Schema::name);
  if (found == data_->schemas.end()) {
    return false;
  }
  Mask mask{};
  for (const auto path : properties) {
    const auto index = propertyIndex(path);
    if (index == PROPERTIES.size() || (mask & (1U << index))) {
      return false;
    }
    mask |= 1U << index;
  }
  return found->mask == mask;
}

std::expected<ResolvedControl, ControlContextError>
ControlContext::resolve(std::string_view name, const ControlSettings &instance) const noexcept {
  const auto checked = validate();

  if (!checked) {
    return std::unexpected(checked.error());
  }

  try {
    if (!identifier(name)) {
      return std::unexpected(error(Code::INVALID_NAME, name));
    }

    if (data_) {
      return resolveData(*data_, name, instance, limits_);
    }

    const Data empty;

    return resolveData(empty, name, instance, limits_);
  } catch (...) {
    return std::unexpected(exceptionError());
  }
}

ControlContextLimits ControlContext::limits() const noexcept {
  return limits_;
}
} // namespace portals::ui
