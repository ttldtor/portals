// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_scene_resource.hpp>

#include <utf8proc.h>

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
    return lineEdit(arguments.text);
  });
  (void)add<Button>("Button", [](SceneResourceArguments arguments) {
    return button(arguments.text);
  });
  (void)add<CheckBox>("CheckBox", [](SceneResourceArguments arguments) {
    return checkbox(arguments.text, arguments.checked);
  });
  entries_.back().checked = true;
}

namespace detail {
/** @brief Internal immutable schema and checked materialization access; no registry borrow escapes prepare. */
struct ResourceAccess {
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

struct ResourceNode {
  // Suppress noexcept string/vector moves on the checked MSVC standard library.
  ~ResourceNode() = default;

  std::size_t type{};
  std::size_t offset{};
  std::size_t textOffset{};
  bool resourceText{};
  bool hasText{};
  SceneResourceArguments arguments;
  std::optional<std::size_t> parent;
};

// Source ownership is shared with failures, so reporting never borrows parser buffers.
struct ResourceDocument {
  std::shared_ptr<const std::string> identity;
  std::string bytes = std::string(0, '\0');
  std::vector<ResourceNode> nodes = std::vector<ResourceNode>(0);
  std::vector<std::size_t> order = std::vector<std::size_t>(0);

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
};

class Parser {
public:
  Parser(ResourceDocument &document, const SceneResourceOptions &options) : document_(document), options_(options) {
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

    // Validate source encoding separately: even ignored comments cannot hide invalid bytes.
    for (std::size_t offset = 0; offset < document_.bytes.size();) {
      utf8proc_int32_t scalar{};
      const auto length = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(document_.bytes.data() + offset),
                                           static_cast<utf8proc_ssize_t>(document_.bytes.size() - offset), &scalar);

      if (length < 0) {
        return std::unexpected(document_.failure(Code::INVALID_UTF8, offset));
      }

      offset += static_cast<std::size_t>(length);
    }

    auto root = node(1);

    if (!root) {
      return std::unexpected(root.error());
    }

    skip();

    if (position_ != document_.bytes.size()) {
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

  std::expected<void, ManagedError> property(ResourceNode &target, std::string_view key, std::size_t offset) {
    const auto &schema = detail::ResourceAccess::entry(options_.registry, target.type);

    if (key == "text" && schema.text) {
      return text(target);
    }

    if (key == "checked" && schema.checked) {
      const auto value = name();

      if (value != "true" && value != "false") {
        return std::unexpected(document_.failure(Code::INVALID_VALUE, offset));
      }

      target.arguments.checked = value == "true";

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
    } else if (key == "height") {
      target.arguments.height = static_cast<std::int32_t>(*value);
    } else if (key == "flex") {
      target.arguments.flex = *value;
    } else {
      target.arguments.gap = static_cast<std::int32_t>(*value);
    }

    return {};
  }

  std::expected<std::size_t, ManagedError> node(std::size_t depth) {
    skip();

    const auto offset = position_;

    if (depth > options_.limits.maxDepth) {
      return std::unexpected(document_.failure(Code::DEPTH_LIMIT, offset));
    }

    if (document_.nodes.size() == options_.limits.maxNodes) {
      return std::unexpected(document_.failure(Code::NODE_LIMIT, offset));
    }

    const auto type = name();
    const auto registered = detail::ResourceAccess::find(options_.registry, type);

    if (type.empty() || !registered) {
      return std::unexpected(document_.failure(type.empty() ? Code::SYNTAX : Code::UNKNOWN_TYPE, offset));
    }

    ResourceNode target;

    target.type = *registered;
    target.offset = offset;

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

    const auto index = document_.nodes.size();

    document_.nodes.emplace_back();

    std::vector<std::string_view> properties(0);

    while (true) {
      skip();

      if (take('}')) {
        document_.nodes[index] = std::move(target);
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

      if (named || positional) {
        const auto key = positional ? std::string_view("text") : token;

        if (properties.size() == options_.limits.maxProperties) {
          return std::unexpected(document_.failure(Code::PROPERTY_LIMIT, begin));
        }

        if (std::ranges::find(properties, key) != properties.end()) {
          return std::unexpected(document_.failure(Code::DUPLICATE_PROPERTY, begin));
        }

        properties.push_back(key);

        if (positional) {
          position_ = begin;
        }

        auto applied = property(target, key, begin);

        if (!applied) {
          return std::unexpected(applied.error());
        }
      } else {
        if (!detail::ResourceAccess::entry(options_.registry, target.type).children) {
          return std::unexpected(document_.failure(Code::CHILDREN_FORBIDDEN, begin));
        }

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
  const SceneResourceOptions &options_;
  std::size_t position_{};
  std::vector<std::pair<std::string_view, std::size_t>> ids_ = std::vector<std::pair<std::string_view, std::size_t>>(0);
};

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
  auto parsed = Parser(document, options).parse();

  if (!parsed) {
    return std::unexpected(parsed.error());
  }

  auto resolved = resolve(document, options, owner.widthPolicy());

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
      auto recipe = detail::ResourceAccess::entry(options.registry, node.type).create(std::move(node.arguments));

      if (!recipe) {
        return std::unexpected(attributed(document, index, recipe.error()));
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

  return detail::Access::transaction(
    [identity = std::move(identity),
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
    });
}

Description<Widget> resource(SceneResourceFile source, SceneResourceOptions options) {
  const auto pathBytes = source.path.generic_u8string();
  auto identity =
    std::make_shared<const std::string>(reinterpret_cast<const char *>(pathBytes.data()), pathBytes.size());

  return detail::Access::transaction(
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
    });
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
