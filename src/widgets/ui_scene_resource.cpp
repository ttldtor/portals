// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/control_context.hpp>
#include <portals/ui_scene_resource.hpp>

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <limits>

namespace portals::ui {
namespace {
bool identifierStart(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool identifierPart(char value) {
  return identifierStart(value) || (value >= '0' && value <= '9');
}

bool identifier(std::string_view value) {
  return !value.empty() && identifierStart(value.front()) && std::ranges::all_of(value.substr(1), identifierPart);
}
} // namespace

std::expected<void, SceneResourceErrorCode> SceneResourceRegistry::insert(Entry entry) {
  if (!identifier(entry.name)) {
    return std::unexpected(SceneResourceErrorCode::UNKNOWN_TYPE);
  }

  if (std::ranges::any_of(entries_, [&](const Entry &existing) {
        return existing.name == entry.name;
      })) {
    return std::unexpected(SceneResourceErrorCode::DUPLICATE_TYPE);
  }

  entries_.push_back(std::move(entry));

  return {};
}

SceneResourceRegistry::SceneResourceRegistry() {
  (void)add<Column>(
    "VBox",
    [](SceneResourceArguments arguments) {
      return column(LinearOptions{.gap = arguments.gap});
    },
    true);
  entries_.back().text = false;
  entries_.back().gap = true;

  (void)add<Row>(
    "HBox",
    [](SceneResourceArguments arguments) {
      return row(LinearOptions{.gap = arguments.gap});
    },
    true);
  entries_.back().text = false;
  entries_.back().gap = true;

  (void)add<Label>("Label", [](SceneResourceArguments arguments) {
    return label(arguments.text);
  });
  (void)add<LineEdit>("LineEdit", [](SceneResourceArguments arguments) {
    auto options = detail::controlLineEditOptions(arguments.settings, {}, {}, {});

    if (!options) {
      return std::expected<Description<LineEdit>, ManagedError>(std::unexpected(options.error()));
    }

    return std::expected<Description<LineEdit>, ManagedError>(
      lineEdit(arguments.text, LineEditConfig{.options = std::move(*options)}));
  });
  (void)add<Button>("Button", [](SceneResourceArguments arguments) {
    return button(arguments.text);
  });
  (void)add<CheckBox>("CheckBox", [](SceneResourceArguments arguments) {
    return checkbox(arguments.text,
                    arguments.settings.state.value_or(arguments.checked ? CheckState::CHECKED : CheckState::UNCHECKED),
                    CheckBoxOptions{.cycle = arguments.settings.cycle.value_or(CheckCycle::TWO_STATE)});
  });
  entries_.back().checked = true;

  (void)add<ThreeState>("ThreeState", [](SceneResourceArguments arguments) {
    return threeState(arguments.text, arguments.settings.state.value_or(CheckState::UNCHECKED));
  });
  for (auto &entry : entries_) {
    entry.factoryKind = detail::ControlFactoryKind::EXISTING_BUILTIN;
  }
}

namespace detail {
/** @brief Internal immutable schema and checked materialization access; no registry borrow escapes prepare. */
struct ResourceAccess {
  static const auto &entries(const SceneResourceRegistry &registry) noexcept {
    return registry.entries_;
  }

  static const auto &registrationFailure(const SceneResourceOptions &options) noexcept {
    return options.registration.failure_;
  }

  /** @param registry Owning allowlist borrowed for one prepare.
   * @param name Case-sensitive source type.
   * @return Stable entry index, or no match; no factory is invoked.
   */
  static std::optional<std::size_t> find(const SceneResourceRegistry &registry, std::string_view name) {
    for (std::size_t index = 0; index < registry.entries_.size(); ++index) {
      if (registry.entries_[index].name == name) {
        return index;
      }
    }

    return {};
  }

  /** @brief Append the bounded default only after caller declarations have claimed their names. */
  static std::expected<void, ControlContextError> installGrid(SceneResourceRegistry &registry, ControlContext &controls,
                                                              bool typed) {
    if (find(registry, "Grid") || ControlContextAccess::hasDeclaredType(controls, "Grid")) {
      return {};
    }

    if (typed) {
      auto installed = ControlContextAccess::installFallbackSchema(controls);
      if (!installed) {
        return installed;
      }
    }

    SceneResourceRegistry::Entry entry;
    entry.name = "Grid";
    entry.children = true;
    entry.sized = true;
    entry.text = false;
    entry.factoryKind = ControlFactoryKind::DECLARATIVE_GRID_FALLBACK;
    registry.entries_.push_back(entry);

    return {};
  }

  /** @param registry Owning allowlist, unchanged during prepare.
   * @param index Index previously validated by find.
   * @return Immutable schema/factory borrowed only for the current synchronous operation.
   */
  static const auto &entry(const SceneResourceRegistry &registry, std::size_t index) {
    return registry.entries_[index];
  }
};
} // namespace detail

namespace {
using Code = SceneResourceErrorCode;

bool valid(const SceneResourceLimits &limits) {
  constexpr std::size_t MAX_RESOURCE_DEPTH = 64;

  return limits.maxSourceBytes && limits.maxNodes && limits.maxDepth && limits.maxDepth <= MAX_RESOURCE_DEPTH &&
         limits.maxStringBytes && limits.maxProperties;
}

void propertyPath(std::string &destination, std::string_view type, std::string_view property) {
  destination.assign(type);

  if (!destination.empty() && !property.empty()) {
    destination.push_back('.');
  }

  destination.append(property);
}

struct ResourceNode {
  // Suppress noexcept string/vector moves on the checked MSVC standard library.
  ~ResourceNode() = default;

  struct FactoryIdentity {
    detail::ControlFactoryKind kind{detail::ControlFactoryKind::USER};
    std::size_t index{};
  } factory;

  std::size_t type{};
  GridItemOptions gridItem;
  bool itemRequested{};
  bool regions{};
  bool headerDeclared{};
  bool bodyDeclared{};
  bool directChildren{};
  bool stickyHeader{};
  std::optional<GridPlacementPlan> gridPlan;
  DeclarativeGridOptions gridOptions;
  std::shared_ptr<Node<Grid>> gridNode;
  std::vector<std::size_t> gridChildren = std::vector<std::size_t>(0);
  std::size_t offset{};
  std::size_t textOffset{};
  bool resourceText{};
  bool hasText{};
  SceneResourceArguments arguments;
  std::string name = std::string(0, '\0');
  std::vector<std::string> lineage = std::vector<std::string>(0);
  std::optional<std::size_t> parent;
};

// Source ownership is shared with failures, so reporting never borrows parser buffers.
struct ResourceDocument {
  std::shared_ptr<const std::string> identity;
  std::string bytes = std::string(0, '\0');
  std::vector<ResourceNode> nodes = std::vector<ResourceNode>(0);
  std::vector<std::size_t> order = std::vector<std::size_t>(0);
  std::vector<std::size_t> lines = std::vector<std::size_t>(0);

  std::shared_ptr<const ControlSourceOrigin> origin(std::size_t offset) const {
    ControlSourceOrigin result;

    result.sharedSourceIdentity = identity;
    result.byteOffset = offset;

    const auto line = std::ranges::upper_bound(lines, offset);

    result.line = static_cast<std::size_t>(line - lines.begin());
    result.column = offset - *(line - 1) + 1;

    return std::make_shared<const ControlSourceOrigin>(std::move(result));
  }

  ManagedError failure(Code code, std::size_t offset, std::error_code system = {}, bool positioned = true) const {
    auto result = detail::error({}, TreeError::INVALID_ARGUMENT);
    SceneResourceError resource{code, identity, offset, 1, 1, system};

    for (std::size_t index = 0; index < std::min(offset, bytes.size()); ++index) {
      if (bytes[index] == '\n') {
        ++resource.line;
        resource.column = 1;
      } else {
        ++resource.column;
      }
    }

    if (!positioned) {
      resource.line = 0;
      resource.column = 0;
    }

    try {
      result.sceneResource = std::make_shared<const SceneResourceError>(std::move(resource));
    } catch (const std::bad_alloc &) {
      // Diagnostic context must never replace the original typed failure under memory pressure.
    }

    return result;
  }

  ManagedError propertyFailure(Code code, std::size_t offset, std::string_view type, std::string_view path) const {
    auto result = failure(code, offset);

    try {
      if (result.sceneResource) {
        auto resource = *result.sceneResource;

        std::string fullPath(0, '\0');

        propertyPath(fullPath, type, path);
        resource.propertyPath = std::make_shared<const std::string>(fullPath);
        result.sceneResource = std::make_shared<const SceneResourceError>(std::move(resource));
      }
    } catch (const std::bad_alloc &) {
      // Optional provenance must not replace the original typed failure.
    }

    return result;
  }
};

class Parser {
public:
  Parser(ResourceDocument &document, SceneResourceOptions &options, ControlContext &controls, bool typed)
      : document_(document), options_(options), controls_(controls), typed_(typed) {
  }

  std::expected<void, ManagedError> parse() {
    const auto &limits = options_.limits;

    if (!valid(limits)) {
      return std::unexpected(document_.failure(Code::INVALID_LIMIT, 0));
    }

    if (document_.bytes.size() > limits.maxSourceBytes ||
        document_.bytes.size() > static_cast<std::size_t>(std::numeric_limits<utf8proc_ssize_t>::max())) {
      return std::unexpected(document_.failure(Code::SOURCE_LIMIT, 0));
    }

    document_.lines.push_back(0);

    // Validate source encoding separately: even ignored comments cannot hide invalid bytes.
    for (std::size_t offset = 0; offset < document_.bytes.size();) {
      utf8proc_int32_t scalar{};
      const auto length = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(document_.bytes.data() + offset),
                                           static_cast<utf8proc_ssize_t>(document_.bytes.size() - offset), &scalar);

      if (length < 0) {
        return std::unexpected(document_.failure(Code::INVALID_UTF8, offset));
      }

      if (scalar == '\n') {
        document_.lines.push_back(offset + 1);
      }

      offset += static_cast<std::size_t>(length);
    }

    std::optional<std::size_t> sceneEnd;

    if (options_.format == SceneResourceFormat::CONFIGURATION) {
      auto configured = configuration();

      if (!configured) {
        return std::unexpected(configured.error());
      }

      position_ = configured->first;
      sceneEnd = configured->second;
    }

    auto fallback = detail::ResourceAccess::installGrid(options_.registry, controls_, typed_);
    if (!fallback) {
      return std::unexpected(contextFailure(fallback.error(), 0));
    }

    auto prepared = controls_.prepared();

    if (!prepared) {
      return std::unexpected(contextFailure(prepared.error(), 0));
    }

    controls_ = *prepared;

    auto root = node(1);

    if (!root) {
      return std::unexpected(root.error());
    }

    skip();

    if (position_ != sceneEnd.value_or(document_.bytes.size())) {
      return std::unexpected(document_.failure(Code::SYNTAX, position_));
    }

    // Views point only into the stationary owning document. Sorting validates all ids
    // without a hash container whose debug proxy constructor can be noexcept on MSVC.
    std::ranges::sort(ids_);

    for (std::size_t index = 1; index < ids_.size(); ++index) {
      if (ids_[index - 1].first == ids_[index].first) {
        return std::unexpected(document_.failure(Code::DUPLICATE_ID, ids_[index].second));
      }
    }

    return {};
  }

private:
  ManagedError withProperty(ManagedError failure, std::string_view type, std::string_view path) const {
    try {
      if (failure.sceneResource) {
        auto resource = *failure.sceneResource;

        std::string fullPath(0, '\0');

        propertyPath(fullPath, type, path);
        resource.propertyPath = std::make_shared<const std::string>(fullPath);
        failure.sceneResource = std::make_shared<const SceneResourceError>(std::move(resource));
      }
    } catch (const std::bad_alloc &) {
      // Existing tree/text/callback errors remain authoritative.
    }

    return failure;
  }

  ManagedError contextFailure(const ControlContextError &cause, std::size_t fallback) const {
    auto failure = detail::controlContextError(cause);

    if (failure.sceneResource && !failure.sceneResource->source) {
      if (cause.diagnostic) {
        const auto path = std::string_view(cause.diagnostic->path);
        const auto type = path.substr(0, path.find('.'));
        const auto found = std::ranges::find(declarations_, type, [](const auto &entry) {
          return entry.first;
        });

        if (found != declarations_.end()) {
          fallback = found->second;
        }
      }

      const auto positioned = document_.failure(failure.sceneResource->code, fallback);

      if (!positioned.sceneResource) {
        return failure;
      }

      auto resource = *positioned.sceneResource;

      resource.propertyPath = failure.sceneResource->propertyPath;
      failure.sceneResource = std::make_shared<const SceneResourceError>(std::move(resource));
    }

    return failure;
  }

  char peek() const {
    return position_ < document_.bytes.size() ? document_.bytes[position_] : '\0';
  }

  void skip() {
    while (position_ < document_.bytes.size()) {
      const auto value = peek();

      if (value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == ',' || value == ';') {
        ++position_;
      } else if (value == '/' && position_ + 1 < document_.bytes.size() && document_.bytes[position_ + 1] == '/') {
        while (position_ < document_.bytes.size() && peek() != '\n') {
          ++position_;
        }
      } else {
        break;
      }
    }
  }

  bool take(char value) {
    skip();

    if (peek() != value) {
      return false;
    }

    ++position_;

    return true;
  }

  std::string_view name(bool id = false) {
    skip();

    const auto begin = position_;

    if (!identifierStart(peek())) {
      return {};
    }

    ++position_;

    while (position_ < document_.bytes.size() && (identifierPart(peek()) || (id && peek() == '-'))) {
      ++position_;
    }

    return std::string_view(document_.bytes).substr(begin, position_ - begin);
  }

  std::expected<void, ManagedError> string(std::string &result) {
    skip();

    const auto begin = position_;

    if (!take('"')) {
      return std::unexpected(document_.failure(Code::SYNTAX, position_));
    }

    while (position_ < document_.bytes.size()) {
      char value = document_.bytes[position_++];

      if (value == '"') {
        return {};
      }

      if (static_cast<unsigned char>(value) < 0x20) {
        return std::unexpected(document_.failure(Code::SYNTAX, position_ - 1));
      }

      if (value == '\\') {
        if (position_ == document_.bytes.size()) {
          break;
        }

        value = document_.bytes[position_++];

        switch (value) {
        case '"':
        case '\\':
          break;
        case 'n':
          value = '\n';
          break;
        case 'r':
          value = '\r';
          break;
        case 't':
          value = '\t';
          break;
        default:
          return std::unexpected(document_.failure(Code::SYNTAX, position_ - 1));
        }
      }

      if (result.size() == options_.limits.maxStringBytes) {
        return std::unexpected(document_.failure(Code::STRING_LIMIT, position_ - 1));
      }

      result.push_back(value);
    }

    return std::unexpected(document_.failure(Code::SYNTAX, begin));
  }

  std::expected<void, ManagedError> text(ResourceNode &target) {
    skip();
    target.textOffset = position_;
    target.hasText = true;

    if (peek() != '"') {
      if (name() != "i18n" || !take('(')) {
        return std::unexpected(document_.failure(Code::SYNTAX, target.textOffset));
      }

      target.resourceText = true;
    }

    auto value = string(target.arguments.text);

    if (!value) {
      return std::unexpected(value.error());
    }

    if (target.resourceText && !take(')')) {
      return std::unexpected(document_.failure(Code::SYNTAX, position_));
    }

    return {};
  }

  std::expected<std::uint32_t, ManagedError> integer(bool positive) {
    skip();

    const auto begin = position_;

    while (peek() >= '0' && peek() <= '9') {
      ++position_;
    }

    std::uint32_t value{};
    const auto *first = document_.bytes.data() + begin;
    const auto *last = document_.bytes.data() + position_;
    const auto parsed = std::from_chars(first, last, value);

    if (parsed.ec != std::errc{} || parsed.ptr != last || first == last || (positive && value == 0) ||
        (!positive && value > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))) {
      return std::unexpected(document_.failure(Code::INVALID_VALUE, begin));
    }

    // Reject numeric expressions and suffixes rather than accepting a valid prefix.
    const char next = peek();

    if (next != '\0' && next != ' ' && next != '\t' && next != '\r' && next != '\n' && next != ',' && next != ';' &&
        next != '}') {
      return std::unexpected(document_.failure(Code::SYNTAX, position_));
    }

    return value;
  }

  std::expected<std::size_t, ManagedError> capacity() {
    skip();

    const auto begin = position_;

    while (peek() >= '0' && peek() <= '9') {
      ++position_;
    }

    std::size_t value{};
    const auto *first = document_.bytes.data() + begin;
    const auto *last = document_.bytes.data() + position_;
    const auto parsed = std::from_chars(first, last, value);

    if (parsed.ec != std::errc{} || parsed.ptr != last || first == last) {
      return std::unexpected(document_.failure(Code::INVALID_VALUE, begin));
    }

    const char next = peek();

    if (next != '\0' && next != ' ' && next != '\t' && next != '\r' && next != '\n' && next != ',' && next != ';' &&
        next != '}') {
      return std::unexpected(document_.failure(Code::SYNTAX, position_));
    }

    return value;
  }

  const ControlPropertyDescriptor *descriptor(std::string_view path) const {
    for (const auto type : {"LineEdit", "CheckBox", "VBox"}) {
      const auto schema = controlPropertySchema(type);
      const auto found = std::ranges::find(schema, path, &ControlPropertyDescriptor::path);

      if (found != schema.end()) {
        return &*found;
      }
    }

    const auto grid = detail::ControlContextAccess::gridSchema();
    const auto found = std::ranges::find(grid, path, &ControlPropertyDescriptor::path);
    return found == grid.end() ? nullptr : &*found;
  }

  std::expected<void, ManagedError> setting(ControlSettings &settings, std::string_view type, std::string_view path,
                                            std::size_t offset) {
    const auto *schema = descriptor(path);

    if (!schema) {
      return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
    }

    if (schema->depth > controls_.limits().maxStructDepth) {
      return std::unexpected(document_.propertyFailure(Code::DEPTH_LIMIT, offset, type, path));
    }

    settings.origins.push_back({std::string(path), document_.origin(offset)});

    if (schema->kind == ControlPropertyKind::TEXT) {
      ResourceNode temporary;
      auto parsed = text(temporary);

      if (!parsed) {
        return std::unexpected(withProperty(parsed.error(), type, path));
      }

      auto value = temporary.resourceText ? i18n(temporary.arguments.text) : TextValue(temporary.arguments.text);

      if (path == "text") {
        settings.text = std::move(value);
      } else {
        settings.placeholder = std::move(value);
      }

      return {};
    }

    if (schema->kind == ControlPropertyKind::BOOLEAN) {
      const auto value = name();

      if (value != "true" && value != "false") {
        return std::unexpected(document_.propertyFailure(Code::INVALID_VALUE, offset, type, path));
      }

      const bool enabled = value == "true";

      if (path == "scroll") {
        settings.grid.scroll = enabled;
      } else if (path == "readOnly") {
        settings.readOnly = enabled;
      } else if (path == "history.enabled") {
        settings.history.enabled = enabled;
      } else if (path == "history.groupTyping") {
        settings.history.groupTyping = enabled;
      } else {
        settings.history.groupDeletion = enabled;
      }

      return {};
    }

    if (schema->kind == ControlPropertyKind::ENUMERATION) {
      const auto value = name();
      const auto found = std::ranges::find(schema->enumNames, value);

      if (found == schema->enumNames.end()) {
        return std::unexpected(document_.propertyFailure(Code::INVALID_VALUE, offset, type, path));
      }

      const auto index = found - schema->enumNames.begin();

      if (path == "state") {
        settings.state = static_cast<CheckState>(index);
      } else if (path == "cycle") {
        settings.cycle = static_cast<CheckCycle>(index);
      } else if (path == "editMode") {
        settings.editMode = static_cast<EditMode>(index);
      } else if (path == "programmaticChange") {
        settings.programmaticChange = static_cast<ProgrammaticChange>(index);
      } else if (path == "flow") {
        settings.grid.flow = static_cast<GridFlow>(index);
      } else {
        settings.displayMode = static_cast<LineEditDisplayMode>(index);
      }

      return {};
    }

    auto value = capacity();

    if (!value) {
      return std::unexpected(withProperty(value.error(), type, path));
    }

    if ((schema->kind == ControlPropertyKind::SIGNED_INTEGER &&
         *value > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) ||
        (path == "flex" && (*value == 0 || *value > std::numeric_limits<std::uint32_t>::max()))) {
      return std::unexpected(document_.propertyFailure(Code::INVALID_VALUE, offset, type, path));
    }

    if (path == "width") {
      settings.width = static_cast<std::int32_t>(*value);
    } else if (path == "height") {
      settings.height = static_cast<std::int32_t>(*value);
    } else if (path == "gap") {
      settings.gap = static_cast<std::int32_t>(*value);
    } else if (path == "flex") {
      settings.flex = static_cast<std::uint32_t>(*value);
    } else if (path == "history.maxOperations") {
      settings.history.maxOperations = *value;
    } else if (path == "history.maxBytes") {
      settings.history.maxBytes = *value;
    } else if (path == "columns") {
      settings.grid.columns = *value;
    } else if (path == "rows") {
      settings.grid.rows = *value;
    } else if (path == "columnGap") {
      settings.grid.columnGap = static_cast<std::int32_t>(*value);
    } else if (path == "rowGap") {
      settings.grid.rowGap = static_cast<std::int32_t>(*value);
    } else if (path == "padding.left") {
      settings.grid.paddingLeft = static_cast<std::int32_t>(*value);
    } else if (path == "padding.top") {
      settings.grid.paddingTop = static_cast<std::int32_t>(*value);
    } else if (path == "padding.right") {
      settings.grid.paddingRight = static_cast<std::int32_t>(*value);
    } else if (path == "padding.bottom") {
      settings.grid.paddingBottom = static_cast<std::int32_t>(*value);
    } else if (path == "maxTracksPerAxis") {
      settings.grid.maxTracksPerAxis = *value;
    } else if (path == "maxPlacements") {
      settings.grid.maxPlacements = *value;
    } else if (path == "maxPlacementWork") {
      settings.grid.maxPlacementWork = *value;
    }

    return {};
  }

  std::expected<void, ManagedError> settingsBody(ControlSettings &settings, std::string_view type,
                                                 std::string_view prefix = {}, std::size_t depth = 1) {
    if (depth > controls_.limits().maxStructDepth) {
      return std::unexpected(document_.propertyFailure(Code::DEPTH_LIMIT, position_, type, prefix));
    }

    std::vector<std::string_view> properties(0);

    while (!take('}')) {
      skip();

      const auto begin = position_;
      const auto key = name();
      std::string path(0, '\0');

      propertyPath(path, prefix, key);

      if (key.empty()) {
        return std::unexpected(document_.propertyFailure(Code::SYNTAX, begin, type, path));
      }

      if (properties.size() == options_.limits.maxProperties) {
        return std::unexpected(document_.propertyFailure(Code::PROPERTY_LIMIT, begin, type, path));
      }

      if (std::ranges::find(properties, key) != properties.end()) {
        return std::unexpected(document_.propertyFailure(Code::DUPLICATE_PROPERTY, begin, type, path));
      }

      properties.push_back(key);

      const bool assigned = take('=') || take(':');

      if (take('{')) {
        if (path != "history" && path != "padding") {
          return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, begin, type, path));
        }

        auto parsed = settingsBody(settings, type, path, depth + 1);

        if (!parsed) {
          return parsed;
        }
      } else {
        if (!assigned) {
          return std::unexpected(document_.propertyFailure(Code::SYNTAX, begin, type, path));
        }

        auto parsed = setting(settings, type, path, begin);

        if (!parsed) {
          return parsed;
        }
      }
    }

    return {};
  }

  std::expected<std::pair<std::size_t, std::size_t>, ManagedError> configuration() {
    struct Block {
      std::string_view kind;
      std::size_t begin;
      std::size_t end;
    };

    std::vector<Block> blocks(0);
    std::optional<std::pair<std::size_t, std::size_t>> scene;

    // Scan source ranges without rewriting bytes: inherited diagnostics keep original positions.
    while (true) {
      skip();

      if (position_ == document_.bytes.size()) {
        break;
      }

      const auto offset = position_;
      const auto kind = name();

      if ((kind != "definitions" && kind != "defaults" && kind != "scene") || !take('{')) {
        return std::unexpected(document_.failure(Code::SYNTAX, offset));
      }

      const auto begin = position_;
      std::size_t braces{1};

      while (braces) {
        skip();

        if (position_ == document_.bytes.size()) {
          return std::unexpected(document_.failure(Code::SYNTAX, position_));
        }

        if (peek() == '"') {
          std::string ignored(0, '\0');
          auto parsed = string(ignored);

          if (!parsed) {
            return std::unexpected(parsed.error());
          }
        } else {
          const auto value = document_.bytes[position_++];

          if (value == '{') {
            ++braces;
          } else if (value == '}') {
            --braces;
          }
        }
      }

      const auto end = position_ - 1;

      if (kind == "scene") {
        if (scene) {
          return std::unexpected(document_.failure(Code::DUPLICATE_PROPERTY, offset));
        }

        scene = std::pair{begin, end};
      } else {
        blocks.push_back({kind, begin, end});
      }
    }

    if (!scene) {
      return std::unexpected(document_.failure(Code::SYNTAX, position_));
    }

    for (const auto &block : blocks) {
      position_ = block.begin;
      skip();

      while (position_ < block.end) {
        const auto offset = position_;
        const auto type = name();
        auto base = type;

        declarations_.emplace_back(type, offset);

        if (type.empty() || (block.kind == "definitions" && !take('='))) {
          return std::unexpected(document_.failure(Code::SYNTAX, offset));
        }

        if (block.kind == "definitions") {
          skip();

          const auto baseOffset = position_;

          base = name();
          declarations_.emplace_back(base, baseOffset);
        }

        if (base.empty() || !take('{')) {
          return std::unexpected(document_.failure(Code::SYNTAX, position_));
        }

        ControlSettings settings;

        settings.origin = document_.origin(offset);

        auto parsed = settingsBody(settings, type);

        if (!parsed) {
          return std::unexpected(parsed.error());
        }

        if (block.kind == "definitions") {
          controls_.define(type, base, settings);
        } else {
          if (std::ranges::find(defaults_, type) != defaults_.end()) {
            return std::unexpected(document_.propertyFailure(Code::DUPLICATE_PROPERTY, offset, type, {}));
          }

          defaults_.push_back(type);
          controls_.overlayDefaults(type, settings);
        }

        skip();
      }

      if (position_ != block.end) {
        return std::unexpected(document_.failure(Code::SYNTAX, position_));
      }
    }

    return *scene;
  }

  std::expected<void, ManagedError> property(ResourceNode &target, std::string_view key, std::size_t offset) {
    const auto &schema = detail::ResourceAccess::entry(options_.registry, target.type);

    if (schema.factoryKind == detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK) {
      return setting(target.arguments.settings, target.name, key, offset);
    }

    if (key == "text" && schema.text) {
      auto parsed = text(target);

      if (parsed && typed_) {
        target.arguments.settings.text =
          target.resourceText ? i18n(target.arguments.text) : TextValue(target.arguments.text);
      }

      return parsed;
    }

    if (key == "checked" && schema.checked) {
      const auto value = name();

      if (value != "true" && value != "false") {
        return std::unexpected(document_.failure(Code::INVALID_VALUE, offset));
      }

      target.arguments.checked = value == "true";
      target.arguments.settings.state = target.arguments.checked ? CheckState::CHECKED : CheckState::UNCHECKED;

      return {};
    }

    const bool sized = schema.sized && (key == "width" || key == "height" || key == "flex");

    if (!sized && !(key == "gap" && schema.gap)) {
      return std::unexpected(document_.failure(Code::UNKNOWN_PROPERTY, offset));
    }

    auto value = integer(key == "flex");

    if (!value) {
      return std::unexpected(value.error());
    }

    if (key == "width") {
      target.arguments.width = static_cast<std::int32_t>(*value);
      target.arguments.settings.width = target.arguments.width;
    } else if (key == "height") {
      target.arguments.height = static_cast<std::int32_t>(*value);
      target.arguments.settings.height = target.arguments.height;
    } else if (key == "flex") {
      target.arguments.flex = *value;
      target.arguments.settings.flex = target.arguments.flex;
    } else {
      target.arguments.gap = static_cast<std::int32_t>(*value);
      target.arguments.settings.gap = target.arguments.gap;
    }

    return {};
  }

  std::expected<void, ManagedError> itemProperty(ResourceNode &target, std::string_view key, std::size_t offset) {
    target.itemRequested = true;
    if (key == "horizontal" || key == "vertical") {
      constexpr std::array<std::string_view, 4> ALIGN_NAMES{"START", "CENTER", "END", "STRETCH"};
      const auto value = name();
      const auto found = std::ranges::find(ALIGN_NAMES, value);
      if (found == ALIGN_NAMES.end()) {
        return std::unexpected(document_.propertyFailure(Code::INVALID_VALUE, offset, target.name, key));
      }

      auto &alignment = key == "horizontal" ? target.gridItem.horizontal : target.gridItem.vertical;
      alignment = static_cast<LayoutAlign>(found - ALIGN_NAMES.begin());

      return {};
    }

    auto value = capacity();
    if (!value) {
      return std::unexpected(withProperty(value.error(), target.name, key));
    }

    if (key == "row") {
      target.gridItem.row = *value;
    } else if (key == "column") {
      target.gridItem.column = *value;
    } else if (key == "rowSpan") {
      target.gridItem.rowSpan = *value;
    } else {
      target.gridItem.columnSpan = *value;
    }

    return {};
  }

  std::expected<void, ManagedError> region(std::size_t parent, GridRegion kind, std::size_t depth, std::size_t offset) {
    const bool declared =
      kind == GridRegion::HEADER ? document_.nodes[parent].headerDeclared : document_.nodes[parent].bodyDeclared;
    if (declared || document_.nodes[parent].directChildren) {
      return std::unexpected(document_.failure(Code::INVALID_VALUE, offset));
    }

    if (kind == GridRegion::HEADER) {
      document_.nodes[parent].headerDeclared = true;
    } else {
      document_.nodes[parent].bodyDeclared = true;
    }

    document_.nodes[parent].regions = true;
    if (!take('{')) {
      return std::unexpected(document_.failure(Code::SYNTAX, position_));
    }

    bool stickyDeclared{};
    while (!take('}')) {
      skip();
      const auto begin = position_;
      const auto token = name();
      const bool assigned = take(':') || take('=');
      if (assigned) {
        if (kind != GridRegion::HEADER || token != "sticky") {
          return std::unexpected(
            document_.propertyFailure(Code::UNKNOWN_PROPERTY, begin, document_.nodes[parent].name, token));
        }

        if (stickyDeclared) {
          return std::unexpected(
            document_.propertyFailure(Code::DUPLICATE_PROPERTY, begin, document_.nodes[parent].name, "header.sticky"));
        }

        stickyDeclared = true;
        const auto value = name();
        if (value != "true" && value != "false") {
          return std::unexpected(
            document_.propertyFailure(Code::INVALID_VALUE, begin, document_.nodes[parent].name, "header.sticky"));
        }

        document_.nodes[parent].stickyHeader = value == "true";
      } else {
        position_ = begin;
        // Structural groups do not consume a widget-node depth or identity.
        auto child = node(depth);
        if (!child) {
          return std::unexpected(child.error());
        }

        document_.nodes[*child].parent = parent;
        document_.nodes[*child].gridItem.region = kind;
      }
    }

    return {};
  }

  std::expected<void, ManagedError> resolveType(std::string_view type, std::size_t offset,
                                                std::optional<std::size_t> &registered) {
    const auto resolved = controls_.resolve(type);

    if (!resolved) {
      return std::unexpected(contextFailure(resolved.error(), offset));
    }

    registered = detail::ResourceAccess::find(options_.registry, resolved->base);

    return {};
  }

  // Header/property error temporaries must not remain in every bounded recursive node frame.
  std::expected<void, ManagedError> nodeProperty(std::size_t index, std::string_view token, bool positional,
                                                 std::size_t begin, std::vector<std::string_view> &properties) {
    auto &target = document_.nodes[index];
    const auto type = std::string_view(target.name);
    const auto key = positional ? std::string_view("text") : token;

    if (properties.size() == options_.limits.maxProperties) {
      return std::unexpected(document_.failure(Code::PROPERTY_LIMIT, begin));
    }

    if (std::ranges::find(properties, key) != properties.end()) {
      return std::unexpected(document_.propertyFailure(Code::DUPLICATE_PROPERTY, begin, type, key));
    }

    properties.push_back(key);

    if (positional) {
      position_ = begin;
    }

    if (key == "row" || key == "column" || key == "rowSpan" || key == "columnSpan" || key == "horizontal" ||
        key == "vertical") {
      return itemProperty(target, key, begin);
    }

    if (key == "padding" && take('{')) {
      return settingsBody(target.arguments.settings, type, "padding", 2);
    }

    if (options_.format != SceneResourceFormat::CONFIGURATION) {
      auto parsed = property(target, key, begin);

      if (parsed && typed_ && target.factory.kind != detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK) {
        target.arguments.settings.origins.push_back(
          {std::string(key == "checked" ? "state" : key), document_.origin(begin)});
      }

      return parsed;
    }

    if (key == "history" && take('{')) {
      return settingsBody(target.arguments.settings, type, "history", 2);
    }

    return setting(target.arguments.settings, type, key, begin);
  }

  std::expected<std::size_t, ManagedError> beginNode(std::size_t depth) {
    skip();

    const auto offset = position_;

    if (depth > options_.limits.maxDepth) {
      return std::unexpected(document_.failure(Code::DEPTH_LIMIT, offset));
    }

    if (document_.nodes.size() == options_.limits.maxNodes) {
      return std::unexpected(document_.failure(Code::NODE_LIMIT, offset));
    }

    const auto type = name();
    auto registered = detail::ResourceAccess::find(options_.registry, type);

    if (typed_ && !type.empty()) {
      const auto checked = resolveType(type, offset, registered);

      if (!checked) {
        return std::unexpected(checked.error());
      }
    }

    if (type.empty() || !registered) {
      return std::unexpected(document_.failure(type.empty() ? Code::SYNTAX : Code::UNKNOWN_TYPE, offset));
    }

    const auto index = document_.nodes.size();

    document_.nodes.emplace_back();

    {
      auto &target = document_.nodes[index];

      target.type = *registered;
      target.factory = {detail::ResourceAccess::entry(options_.registry, *registered).factoryKind, *registered};
      target.offset = offset;
      target.name = type;

      if (typed_) {
        target.arguments.settings.origin = document_.origin(offset);
      }

      if (take('#')) {
        skip();
        const auto idOffset = position_;
        const auto id = name(true);

        if (id.empty()) {
          return std::unexpected(document_.failure(Code::SYNTAX, idOffset));
        }

        target.arguments.id = id;

        ids_.emplace_back(id, idOffset);
      }

      if (!take('{')) {
        return std::unexpected(document_.failure(Code::SYNTAX, position_));
      }
    }

    return index;
  }

  std::expected<std::size_t, ManagedError> node(std::size_t depth) {
    auto begun = beginNode(depth);

    if (!begun) {
      return std::unexpected(begun.error());
    }

    const auto index = *begun;

    std::vector<std::string_view> properties(0);

    while (true) {
      skip();

      if (take('}')) {
        document_.order.push_back(index);

        return index;
      }

      if (position_ == document_.bytes.size()) {
        return std::unexpected(document_.failure(Code::SYNTAX, position_));
      }

      const auto begin = position_;
      const auto token = peek() == '"' ? std::string_view{} : name();
      const bool named = take(':') || take('=');
      const bool positional = !named && (token.empty() || token == "i18n");
      const bool structured =
        (options_.format == SceneResourceFormat::CONFIGURATION && token == "history" && peek() == '{') ||
        (document_.nodes[index].factory.kind == detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK &&
         token == "padding" && peek() == '{');

      if (named || positional || structured) {
        auto applied = nodeProperty(index, token, positional, begin, properties);

        if (!applied) {
          return std::unexpected(applied.error());
        }
      } else {
        const bool standardGrid =
          document_.nodes[index].factory.kind == detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK;
        if (standardGrid && (token == "header" || token == "body")) {
          auto parsed = region(index, token == "header" ? GridRegion::HEADER : GridRegion::BODY, depth + 1, begin);
          if (!parsed) {
            return std::unexpected(parsed.error());
          }
          continue;
        }
        if (standardGrid && document_.nodes[index].regions) {
          return std::unexpected(document_.failure(Code::INVALID_VALUE, begin));
        }
        if (!detail::ResourceAccess::entry(options_.registry, document_.nodes[index].type).children) {
          return std::unexpected(document_.failure(Code::CHILDREN_FORBIDDEN, begin));
        }
        document_.nodes[index].directChildren = true;

        position_ = begin;

        auto child = node(depth + 1);

        if (!child) {
          return std::unexpected(child.error());
        }

        document_.nodes[*child].parent = index;
      }
    }
  }

  ResourceDocument &document_;
  SceneResourceOptions &options_;
  ControlContext &controls_;
  bool typed_{};
  std::size_t position_{};
  std::vector<std::pair<std::string_view, std::size_t>> ids_ = std::vector<std::pair<std::string_view, std::size_t>>(0);
  std::vector<std::pair<std::string_view, std::size_t>> declarations_ =
    std::vector<std::pair<std::string_view, std::size_t>>(0);
  std::vector<std::string_view> defaults_ = std::vector<std::string_view>(0);
};

std::expected<void, ManagedError> registerSchemas(ControlContext &controls, const SceneResourceOptions &options) {
  for (const auto &entry : detail::ResourceAccess::entries(options.registry)) {
    if (!controlPropertySchema(entry.name).empty()) {
      continue;
    }

    std::vector<std::string_view> paths(0);

    if (entry.text) {
      paths.push_back("text");
    }

    if (entry.sized) {
      paths.insert(paths.end(), {"width", "height", "flex"});
    }

    if (entry.gap) {
      paths.push_back("gap");
    }

    if (entry.checked) {
      paths.insert(paths.end(), {"state", "cycle"});
    }

    if (controls.hasRegisteredType(entry.name)) {
      if (!controls.matchesRegisteredType(entry.name, paths)) {
        return std::unexpected(detail::controlContextError({ControlContextErrorCode::DUPLICATE_TYPE, {}}));
      }
    } else {
      controls.registerType(entry.name, paths);
    }
  }

  return {};
}

std::expected<void, ManagedError> prepareNodes(ResourceDocument &document, const ControlContext &controls,
                                               detail::MountContext &context) {
  std::size_t leaves{};
  const auto limit = controls.limits().maxMaterializedLeaves;

  for (auto &node : document.nodes) {
    auto resolved = controls.resolve(node.name, node.arguments.settings);

    if (!resolved) {
      return std::unexpected(detail::controlContextError(resolved.error()));
    }

    if (context.controlLeaves > limit || leaves > limit - context.controlLeaves ||
        resolved->materializedLeaves > limit - context.controlLeaves - leaves) {
      return std::unexpected(document.failure(Code::MATERIALIZATION_LIMIT, node.offset));
    }

    leaves += resolved->materializedLeaves;
    node.lineage = std::move(resolved->lineage);
    node.arguments.settings = std::move(resolved->settings);
    node.arguments.width = node.arguments.settings.width;
    node.arguments.height = node.arguments.settings.height;
    node.arguments.flex = node.arguments.settings.flex;
    node.arguments.gap = node.arguments.settings.gap.value_or(0);
    node.arguments.checked = node.arguments.settings.state == CheckState::CHECKED;
  }

  // Reserve the complete resource before any resolver/factory effect. The enclosing
  // transaction owns rollback; sibling resources share this same aggregate counter.
  context.controlLeaves += leaves;

  return {};
}

DeclarativeGridOptions gridSettings(const ControlGridSettings &settings) {
  DeclarativeGridOptions result;
  result.columns = settings.columns;
  result.rows = settings.rows;
  result.flow = settings.flow.value_or(result.flow);
  result.columnGap = settings.columnGap.value_or(result.columnGap);
  result.rowGap = settings.rowGap.value_or(result.rowGap);
  result.padding = {settings.paddingLeft.value_or(0), settings.paddingTop.value_or(0),
                    settings.paddingRight.value_or(0), settings.paddingBottom.value_or(0)};
  result.maxTracksPerAxis = settings.maxTracksPerAxis.value_or(result.maxTracksPerAxis);
  result.maxPlacements = settings.maxPlacements.value_or(result.maxPlacements);
  result.maxPlacementWork = settings.maxPlacementWork.value_or(result.maxPlacementWork);

  return result;
}

std::expected<void, ManagedError> prepareGrids(ResourceDocument &document, bool typed) {
  ControlContext fallback;
  if (!typed) {
    auto installed = detail::ControlContextAccess::installFallbackSchema(fallback);
    if (!installed) {
      return std::unexpected(detail::controlContextError(installed.error()));
    }
  }

  for (std::size_t index = 0; index < document.nodes.size(); ++index) {
    auto &node = document.nodes[index];
    if (node.parent &&
        document.nodes[*node.parent].factory.kind == detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK) {
      document.nodes[*node.parent].gridChildren.push_back(index);
    }

    if (node.itemRequested && (!node.parent || document.nodes[*node.parent].factory.kind !=
                                                 detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK)) {
      return std::unexpected(document.propertyFailure(Code::INVALID_VALUE, node.offset, node.name, "row"));
    }
  }

  for (auto &node : document.nodes) {
    if (node.factory.kind != detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK) {
      continue;
    }

    if (!typed) {
      auto checked = fallback.resolve("Grid", node.arguments.settings);
      if (!checked) {
        return std::unexpected(detail::controlContextError(checked.error()));
      }

      node.arguments.width = node.arguments.settings.width;
      node.arguments.height = node.arguments.settings.height;
      node.arguments.flex = node.arguments.settings.flex;
    }

    node.gridOptions = gridSettings(node.arguments.settings.grid);
    node.gridOptions.regions = node.regions;
    node.gridOptions.stickyHeader = node.stickyHeader;
    std::vector<GridItemOptions> items(0);
    items.reserve(node.gridChildren.size());
    for (const auto child : node.gridChildren) {
      items.push_back(document.nodes[child].gridItem);
    }

    auto resolved = resolveGridPlacements(node.gridOptions, items);
    if (!resolved) {
      auto failure = document.failure(resolved.error() == TreeError::NODE_LIMIT  ? Code::NODE_LIMIT
                                      : resolved.error() == TreeError::NO_MEMORY ? Code::NO_MEMORY
                                                                                 : Code::INVALID_VALUE,
                                      node.offset);
      failure.cause = resolved.error();

      return std::unexpected(std::move(failure));
    }

    node.gridPlan = *resolved;
    node.gridNode = std::make_shared<Node<Grid>>();
  }

  return {};
}

GridOptions boundGridOptions(const ResourceDocument &document, const ResourceNode &node,
                             const std::vector<Node<Widget>> &mounted) {
  GridOptions result;
  result.columns.resize(node.gridPlan->columns);
  result.rows.resize(node.gridPlan->rows);
  result.header_rows.resize(node.gridPlan->headerRows);
  result.sticky_header = node.gridOptions.stickyHeader;
  result.regions = node.gridOptions.regions;
  result.column_gap = node.gridOptions.columnGap;
  result.row_gap = node.gridOptions.rowGap;
  result.padding = node.gridOptions.padding;
  result.max_tracks_per_axis = node.gridOptions.maxTracksPerAxis;
  result.max_placements = node.gridOptions.maxPlacements;
  result.cells.reserve(node.gridChildren.size());
  for (std::size_t index = 0; index < node.gridChildren.size(); ++index) {
    const auto child = node.gridChildren[index];
    const auto &item = document.nodes[child].gridItem;
    const auto position = node.gridPlan->positions[index];
    result.cells.push_back({mounted[child].id(), position.row, position.column, item.rowSpan, item.columnSpan,
                            item.horizontal, item.vertical, item.region});
  }

  return result;
}

std::expected<Description<Widget>, ManagedError> gridRecipe(ResourceNode &node) {
  auto recipe = grid()
                  .gridOptions(node.gridOptions)
                  .setup([slot = node.gridNode](ManagedScene &, Node<Grid> node) -> std::expected<void, ManagedError> {
                    *slot = node;

                    return {};
                  });
  if (node.arguments.settings.grid.scroll.value_or(false)) {
    (void)std::move(recipe).scroll();
  }

  if (!node.arguments.id.empty()) {
    (void)std::move(recipe).id(node.arguments.id);
  }

  if (node.arguments.width) {
    (void)std::move(recipe).width(*node.arguments.width);
  }

  if (node.arguments.height) {
    (void)std::move(recipe).height(*node.arguments.height);
  }

  if (node.arguments.flex) {
    (void)std::move(recipe).flex(*node.arguments.flex);
  }

  detail::Access::preparedControl(recipe, detail::controlSettingsPresence(node.arguments.settings));

  return detail::Access::erase(std::move(recipe));
}

ManagedError textFailure(ResourceDocument &document, const ResourceNode &node, std::string_view path,
                         const TextValueError *cause = nullptr) {
  auto failure = document.propertyFailure(Code::TEXT, node.offset, node.name, path);
  const auto &settings = node.arguments.settings;
  const auto found = std::ranges::find(settings.origins, path, &ControlPropertyOrigin::path);
  const auto origin = found == settings.origins.end() ? settings.origin : found->source;

  try {
    if (origin && failure.sceneResource) {
      auto resource = *failure.sceneResource;

      resource.source = origin->sharedSourceIdentity
                          ? origin->sharedSourceIdentity
                          : std::shared_ptr<const std::string>(origin, &origin->sourceIdentity);
      resource.offset = origin->byteOffset;
      resource.line = origin->line;
      resource.column = origin->column;
      failure.sceneResource = std::make_shared<const SceneResourceError>(std::move(resource));
    }
  } catch (const std::bad_alloc &) {
    // The text/resource cause below is retained even when enrichment cannot allocate.
  }

  if (cause) {
    if (const auto *text = std::get_if<TextError>(cause)) {
      failure.text = *text;
    } else {
      failure.resource = std::get<TextResourceError>(*cause);
    }
  }

  return failure;
}

std::expected<void, ManagedError> resolveSettings(ResourceDocument &document, const SceneResourceOptions &options,
                                                  WidthPolicy policy) {
  TextLimits limits;

  limits.max_input_bytes = options.limits.maxStringBytes;

  for (auto &node : document.nodes) {
    for (const auto path : {std::string_view("text"), std::string_view("placeholder")}) {
      auto &setting = path == "text" ? node.arguments.settings.text : node.arguments.settings.placeholder;

      if (!setting) {
        continue;
      }

      auto value =
        options.resolver ? setting->resolve(policy, options.resolver, limits) : setting->resolve(policy, limits);

      if (!value) {
        return std::unexpected(textFailure(document, node, path, &value.error()));
      }

      if (path == "placeholder" && portals::detail::hasLineSeparator(value->bytes())) {
        return std::unexpected(textFailure(document, node, path));
      }

      setting = TextValue(value->bytes());

      if (path == "text") {
        node.arguments.text = value->bytes();
      }
    }
  }

  return {};
}

std::expected<void, ManagedError> resolve(ResourceDocument &document, const SceneResourceOptions &options,
                                          WidthPolicy policy) {
  for (auto &node : document.nodes) {
    if (!node.hasText) {
      continue;
    }

    TextLimits limits;

    limits.max_input_bytes = options.limits.maxStringBytes;

    auto value = [&] {
      if (node.resourceText) {
        if (options.resolver) {
          return i18n(node.arguments.text).resolve(policy, options.resolver, limits);
        }

        return i18n(node.arguments.text).resolve(policy, limits);
      }

      return TextValue(node.arguments.text).resolve(policy, limits);
    }();

    if (!value) {
      auto failure = document.failure(Code::TEXT, node.textOffset);

      if (const auto *text = std::get_if<TextError>(&value.error())) {
        failure.text = *text;
      } else {
        failure.resource = std::get<TextResourceError>(value.error());
      }

      return std::unexpected(failure);
    }

    node.arguments.text = value->bytes();
  }

  return {};
}

ManagedError attributed(ResourceDocument &document, std::size_t index, ManagedError failure) {
  if (!failure.sceneResource) {
    failure.sceneResource = document.failure(Code::FACTORY, document.nodes[index].offset).sceneResource;
  }

  return failure;
}

std::expected<Node<Widget>, ManagedError> compile(ManagedScene &owner, ResourceDocument &document,
                                                  const SceneResourceOptions &options, Node<Widget> parent,
                                                  detail::MountContext &context) try {
  auto effectiveOptions = options;
  auto controls = options.controls.value_or(context.controls);
  const bool typed = options.format == SceneResourceFormat::CONFIGURATION ||
                     detail::ControlContextAccess::hasUserConfiguration(controls);

  if (typed) {
    auto registered = registerSchemas(controls, options);

    if (!registered) {
      return std::unexpected(registered.error());
    }
  }

  auto parsed = Parser(document, effectiveOptions, controls, typed).parse();

  if (!parsed) {
    return std::unexpected(parsed.error());
  }

  if (typed) {
    auto prepared = prepareNodes(document, controls, context);

    if (!prepared) {
      return std::unexpected(prepared.error());
    }
  }

  auto placements = prepareGrids(document, typed);

  if (!placements) {
    return std::unexpected(placements.error());
  }

  auto resolved =
    typed ? resolveSettings(document, options, owner.widthPolicy()) : resolve(document, options, owner.widthPolicy());

  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  std::vector<Description<Widget>> recipes(document.nodes.size());

  // Materialize source postorder, but keep every resource edge as an index. The existing
  // typed descriptions mount one at a time in the same transaction rather than recursively
  // multiplying the type-erasure adapters at every source level.
  for (auto index : document.order) {
    auto &node = document.nodes[index];

    try {
      auto recipe = node.factory.kind == detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK
                      ? gridRecipe(node)
                      : detail::ResourceAccess::entry(effectiveOptions.registry, node.factory.index)
                          .create(std::move(node.arguments));

      if (!recipe) {
        return std::unexpected(attributed(document, index, recipe.error()));
      }

      if (typed) {
        *recipe = std::move(*recipe).setup([identity = StyleTarget{.type = node.name, .baseTypes = node.lineage}](
                                             ManagedScene &scene, Node<Widget> node) {
          return detail::Access::finalizeStyleIdentity(scene, node, identity);
        });
      }

      recipes[index] = std::move(*recipe);
    } catch (const std::bad_alloc &) {
      return std::unexpected(attributed(document, index, detail::error({}, TreeError::NO_MEMORY)));
    } catch (...) {
      return std::unexpected(attributed(document, index, detail::error({}, TreeError::CALLBACK_FAILED)));
    }
  }

  const auto existingNodes = context.nodes.size();
  std::vector<Node<Widget>> mounted(document.nodes.size());
  const auto inherited = context.controls;

  struct RestoreContext {
    detail::MountContext &context;
    ControlContext inherited;

    ~RestoreContext() {
      context.controls = inherited;
    }
  } restore{context, inherited};

  context.controls = controls;

  for (std::size_t index = 0; index < recipes.size(); ++index) {
    const auto target = document.nodes[index].parent ? mounted[*document.nodes[index].parent] : parent;
    auto node = detail::Access::mount(std::move(recipes[index]), owner, target, context);

    if (!node) {
      return std::unexpected(attributed(document, index, node.error()));
    }

    mounted[index] = *node;

    auto checked = detail::Access::check(owner, *node);

    if (!checked) {
      return std::unexpected(attributed(document, index, checked.error()));
    }
  }

  for (std::size_t index = 0; index < document.nodes.size(); ++index) {
    auto &node = document.nodes[index];
    if (node.gridPlan) {
      auto configured = detail::Access::gridOptions(owner, *node.gridNode, boundGridOptions(document, node, mounted));
      if (!configured) {
        return std::unexpected(attributed(document, index, configured.error()));
      }
    }
  }

  for (std::size_t index = existingNodes; index < context.nodes.size(); ++index) {
    auto checked = detail::Access::check(owner, context.nodes[index]);

    if (!checked) {
      return std::unexpected(attributed(document, 0, checked.error()));
    }
  }

  return mounted.front();
} catch (const std::bad_alloc &) {
  auto failure = document.failure(Code::NO_MEMORY, 0);

  failure.cause = TreeError::NO_MEMORY;

  return std::unexpected(std::move(failure));
} catch (...) {
  auto failure = document.failure(Code::FACTORY, 0);

  failure.cause = TreeError::CALLBACK_FAILED;

  return std::unexpected(std::move(failure));
}
} // namespace

Description<Widget> resource(std::string_view source, SceneResourceOptions options) {
  auto identity = std::make_shared<const std::string>(source);

  return detail::Access::deferControlValidation(
    detail::Access::transaction([identity = std::move(identity), options = std::move(options)](
                                  ManagedScene &owner, Node<Widget> parent,
                                  detail::MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
      ResourceDocument document{identity};

      try {
        if (const auto &registration = detail::ResourceAccess::registrationFailure(options); registration) {
          auto failure = document.failure(registration->first, 0, {}, false);

          failure.cause = registration->second;

          return std::unexpected(std::move(failure));
        }

        if (!valid(options.limits)) {
          return std::unexpected(document.failure(Code::INVALID_LIMIT, 0));
        }

        if (identity->size() > options.limits.maxSourceBytes) {
          return std::unexpected(document.failure(Code::SOURCE_LIMIT, 0));
        }

        document.bytes = *identity;

        return compile(owner, document, options, parent, context);
      } catch (const std::bad_alloc &) {
        auto failure = document.failure(Code::NO_MEMORY, 0);

        failure.cause = TreeError::NO_MEMORY;

        return std::unexpected(std::move(failure));
      }
    }));
}

Description<Widget> resource(SceneResourceFile source, SceneResourceOptions options) {
  const auto pathBytes = source.path.generic_u8string();
  auto identity =
    std::make_shared<const std::string>(reinterpret_cast<const char *>(pathBytes.data()), pathBytes.size());

  return detail::Access::deferControlValidation(detail::Access::transaction(
    [source = std::move(source), identity = std::move(identity),
     options = std::move(options)](ManagedScene &owner, Node<Widget> parent,
                                   detail::MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
      ResourceDocument document{identity};

      try {
        if (const auto &registration = detail::ResourceAccess::registrationFailure(options); registration) {
          auto failure = document.failure(registration->first, 0, {}, false);

          failure.cause = registration->second;

          return std::unexpected(std::move(failure));
        }

        if (!valid(options.limits)) {
          return std::unexpected(document.failure(Code::INVALID_LIMIT, 0));
        }

        errno = 0;

        std::ifstream input(source.path, std::ios::binary);

        if (!input) {
          return std::unexpected(
            document.failure(Code::FILE_IO, 0, std::error_code(errno ? errno : EIO, std::generic_category())));
        }

        // Read a bounded extra byte to distinguish an exact cap from a truncated resource.
        std::array<char, 4096> buffer{};

        while (input && document.bytes.size() <= options.limits.maxSourceBytes) {
          const auto remaining = options.limits.maxSourceBytes - document.bytes.size();
          const auto count = remaining < buffer.size() ? remaining + 1 : buffer.size();

          input.read(buffer.data(), static_cast<std::streamsize>(count));
          document.bytes.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
        }

        if (input.bad() || (input.fail() && !input.eof())) {
          return std::unexpected(document.failure(Code::FILE_IO, document.bytes.size(),
                                                  std::error_code(errno ? errno : EIO, std::generic_category())));
        }

        return compile(owner, document, options, parent, context);
      } catch (const std::bad_alloc &) {
        auto failure = document.failure(Code::NO_MEMORY, 0);

        failure.cause = TreeError::NO_MEMORY;

        return std::unexpected(std::move(failure));
      } catch (const std::filesystem::filesystem_error &failure) {
        return std::unexpected(document.failure(Code::FILE_IO, 0, failure.code()));
      }
    }));
}

Scene scene(std::string_view source, SceneResourceOptions resourceOptions, SceneOptions options,
            std::source_location origin) {
  return scene(resource(source, std::move(resourceOptions)), options, origin);
}

Scene scene(SceneResourceFile source, SceneResourceOptions resourceOptions, SceneOptions options,
            std::source_location origin) {
  return scene(resource(std::move(source), std::move(resourceOptions)), options, origin);
}
} // namespace portals::ui
