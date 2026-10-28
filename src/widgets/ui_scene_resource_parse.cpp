// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "ui_scene_resource_internal.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

namespace portals::ui::detail::resource {
namespace {
void propertyPath(std::string &destination, std::string_view type, std::string_view property) {
  destination.assign(type);

  if (!destination.empty() && !property.empty()) {
    destination.push_back('.');
  }

  destination.append(property);
}

class Parser {
  using NodeStatus = std::expected<std::size_t, const ManagedError *>;
  using RegionStatus = std::expected<void, const ManagedError *>;

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

    if (options_.format == SceneResourceFormat::CONFIGURATION) {
      auto primitives = detail::ResourceAccess::installPrimitives(options_.registry, controls_);

      if (!primitives) {
        return std::unexpected(contextFailure(primitives.error(), 0));
      }

      auto scrolls = detail::ResourceAccess::installScrolls(options_.registry, controls_);

      if (!scrolls) {
        return std::unexpected(contextFailure(scrolls.error(), 0));
      }
      auto keyHelp = detail::ResourceAccess::installKeyHelp(options_.registry, controls_);
      if (!keyHelp) {
        return std::unexpected(contextFailure(keyHelp.error(), 0));
      }
      auto dialogs = detail::ResourceAccess::installDialogs(options_.registry, controls_);

      if (!dialogs) {
        return std::unexpected(contextFailure(dialogs.error(), 0));
      }

      auto dataState = detail::ResourceAccess::installDataState(options_.registry, controls_);
      if (!dataState) {
        return std::unexpected(contextFailure(dataState.error(), 0));
      }
      auto overlay = detail::ResourceAccess::installOverlay(options_.registry, controls_);
      if (!overlay) {
        return std::unexpected(contextFailure(overlay.error(), 0));
      }
    }

    auto prepared = controls_.prepared();

    if (!prepared) {
      return std::unexpected(contextFailure(prepared.error(), 0));
    }

    controls_ = *prepared;

    auto root = node(1);

    if (!root) {
      return std::unexpected(*root.error());
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

  std::string_view propertyName() {
    const auto first = name();

    if (first.empty()) {
      return first;
    }

    const auto begin = position_ - first.size();

    while (peek() == '.') {
      ++position_;

      if (!identifierStart(peek())) {
        return {};
      }

      ++position_;

      while (identifierPart(peek())) {
        ++position_;
      }
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

  template <class Unsigned = std::size_t> std::expected<Unsigned, ManagedError> capacity() {
    skip();

    const auto begin = position_;

    while (peek() >= '0' && peek() <= '9') {
      ++position_;
    }

    Unsigned value{};
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

  std::expected<double, ManagedError> floating() {
    skip();

    const auto begin = position_;

    while (peek() != '\0' && peek() != ' ' && peek() != '\t' && peek() != '\r' && peek() != '\n' && peek() != ',' &&
           peek() != ';' && peek() != '}') {
      ++position_;
    }

    const auto *first = document_.bytes.data() + begin;
    const auto *last = document_.bytes.data() + position_;
    double value{};
    const auto parsed = std::from_chars(first, last, value, std::chars_format::general);

    if (parsed.ec != std::errc{} || first == last || !std::isfinite(value)) {
      return std::unexpected(document_.failure(Code::INVALID_VALUE, begin));
    }

    if (parsed.ptr != last) {
      return std::unexpected(document_.failure(Code::SYNTAX, begin + static_cast<std::size_t>(parsed.ptr - first)));
    }

    return value;
  }

  const ControlPropertyDescriptor *descriptor(std::string_view path) const {
    for (const auto type : {"FindBar",    "FormField",     "Password",      "LineEdit",
                            "ComboBox",   "DropDown",      "CheckBox",      "VBox",
                            "Button",     "Border",        "Gauge",         "Spinner",
                            "ScrollArea", "ScrollBar",     "LogView",       "Overlay",
                            "HelpView",   "KeyHints",      "VirtualList",   "VirtualTable",
                            "Chart",      "DataStateView", "OverlayHost",   "Dialog",
                            "MessageBox", "Notifications", "ThemeSelector", "ThemeSelectorCatalog"}) {
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

  std::expected<void, ManagedError> compactFrame(ControlSettings &settings, std::string_view type, std::size_t offset,
                                                 std::vector<std::string_view> &properties) {
    constexpr std::array<std::string_view, 2> LEAVES{"frame.enabled", "frame.pattern"};

    for (const auto path : LEAVES) {
      if (std::ranges::find(settings.origins, path, &ControlPropertyOrigin::path) != settings.origins.end()) {
        return std::unexpected(document_.propertyFailure(Code::DUPLICATE_PROPERTY, offset, type, path));
      }

      const auto *schema = descriptor(path);

      if (!schema || schema->depth > controls_.limits().maxStructDepth) {
        return std::unexpected(document_.propertyFailure(Code::DEPTH_LIMIT, offset, type, path));
      }
    }

    // The compact spelling consumes the same two canonical leaves as explicit assignments.
    if (properties.size() > options_.limits.maxProperties ||
        LEAVES.size() > options_.limits.maxProperties - properties.size()) {
      return std::unexpected(document_.propertyFailure(Code::PROPERTY_LIMIT, offset, type, "frame.pattern"));
    }

    std::string source;
    auto parsed = string(source);

    if (!parsed) {
      return std::unexpected(withProperty(parsed.error(), type, "frame.pattern"));
    }

    const auto pattern = detail::framePattern(source);

    if (!pattern) {
      return std::unexpected(document_.propertyFailure(Code::INVALID_VALUE, offset, type, "frame.pattern"));
    }

    for (const auto path : LEAVES) {
      properties.push_back(path);
      settings.origins.push_back({std::string(path), document_.origin(offset)});
    }

    settings.frame.enabled = true;
    settings.frame.pattern = *pattern;

    return {};
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

    if (path.starts_with("frame.") &&
        std::ranges::find(settings.origins, path, &ControlPropertyOrigin::path) != settings.origins.end()) {
      return std::unexpected(document_.propertyFailure(Code::DUPLICATE_PROPERTY, offset, type, path));
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
      } else if (path == "document") {
        settings.findDocument = std::move(value);
      } else if (path == "previousLabel") {
        settings.findPreviousLabel = std::move(value);
      } else if (path == "nextLabel") {
        settings.findNextLabel = std::move(value);
      } else if (path == "clearLabel") {
        settings.findClearLabel = std::move(value);
      } else if (path == "help") {
        settings.fieldHelp = std::move(value);
      } else if (path == "requiredMessage") {
        settings.fieldRequiredMessage = std::move(value);
      } else if (path == "label") {
        settings.spinnerLabel = std::move(value);
      } else if (path == "placeholder") {
        settings.placeholder = std::move(value);
      } else if (path == "title") {
        settings.title = std::move(value);
      } else if (path == "frame.title") {
        settings.frame.title = std::move(value);
      } else if (path == "body") {
        settings.body = std::move(value);
      } else if (path == "themeSelector.labels.apply") {
        settings.themeSelector.labels.apply = std::move(value);
      } else if (path == "themeSelector.labels.cancel") {
        settings.themeSelector.labels.cancel = std::move(value);
      } else if (path == "themeSelector.labels.closeKeepingPreview") {
        settings.themeSelector.labels.closeKeepingPreview = std::move(value);
      } else if (path == "themeSelector.labels.emptyStatus") {
        settings.themeSelector.labels.emptyStatus = std::move(value);
      } else if (path == "themeSelector.labels.errorStatus") {
        settings.themeSelector.labels.errorStatus = std::move(value);
      } else if (path == "themeSelector.labels.conflictStatus") {
        settings.themeSelector.labels.conflictStatus = std::move(value);
      } else if (path == "message.labels.acknowledge") {
        settings.message.labels.acknowledge = std::move(value);
      } else if (path == "message.labels.confirm") {
        settings.message.labels.confirm = std::move(value);
      } else if (path == "message.labels.save") {
        settings.message.labels.save = std::move(value);
      } else if (path == "message.labels.discard") {
        settings.message.labels.discard = std::move(value);
      } else if (path == "message.labels.yes") {
        settings.message.labels.yes = std::move(value);
      } else if (path == "message.labels.no") {
        settings.message.labels.no = std::move(value);
      } else if (path == "message.labels.cancel") {
        settings.message.labels.cancel = std::move(value);
      } else if (path == "labels.loading") {
        settings.labels.loading = std::move(value);
      } else if (path == "labels.empty") {
        settings.labels.empty = std::move(value);
      } else if (path == "labels.error") {
        settings.labels.error = std::move(value);
      } else if (path == "labels.offline") {
        settings.labels.offline = std::move(value);
      } else if (path == "labels.refreshing") {
        settings.labels.refreshing = std::move(value);
      } else if (path == "labels.retry") {
        settings.labels.retry = std::move(value);
      } else {
        return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
      }

      return {};
    }

    if (schema->kind == ControlPropertyKind::LITERAL_STRING) {
      std::string value;
      auto parsed = string(value);
      if (!parsed) {
        return std::unexpected(withProperty(parsed.error(), type, path));
      }
      if (path == "frames") {
        settings.spinnerFrames = std::move(value);
      } else if (path == "themeCatalog.initialId.value") {
        settings.themeCatalog.initialId.value = std::move(value);
      } else if (path == "themeSelector.openingShortcut.value") {
        settings.themeSelector.openingShortcut.value = std::move(value);
      } else if (path == "format.strokeSeparator") {
        settings.format.strokeSeparator = std::move(value);
      } else if (path == "format.alternativeSeparator") {
        settings.format.alternativeSeparator = std::move(value);
      } else if (path == "format.repeatMarker") {
        settings.format.repeatMarker = std::move(value);
      } else if (path == "format.releaseMarker") {
        settings.format.releaseMarker = std::move(value);
      } else if (path == "format.unknownMarker") {
        settings.format.unknownMarker = std::move(value);
      } else if (path == "scrollbar.horizontal.thumbStart") {
        settings.scrollbar.horizontal.thumbStart = std::move(value);
      } else if (path == "scrollbar.horizontal.thumbBody") {
        settings.scrollbar.horizontal.thumbBody = std::move(value);
      } else if (path == "scrollbar.horizontal.thumbEnd") {
        settings.scrollbar.horizontal.thumbEnd = std::move(value);
      } else if (path == "scrollbar.horizontal.thumbSingle") {
        settings.scrollbar.horizontal.thumbSingle = std::move(value);
      } else if (path == "scrollbar.vertical.thumbStart") {
        settings.scrollbar.vertical.thumbStart = std::move(value);
      } else if (path == "scrollbar.vertical.thumbBody") {
        settings.scrollbar.vertical.thumbBody = std::move(value);
      } else if (path == "scrollbar.vertical.thumbEnd") {
        settings.scrollbar.vertical.thumbEnd = std::move(value);
      } else if (path == "scrollbar.vertical.thumbSingle") {
        settings.scrollbar.vertical.thumbSingle = std::move(value);
      } else if (path == "scrollbar.horizontal.track") {
        settings.scrollbar.horizontal.track = std::move(value);
      } else if (path == "scrollbar.horizontal.thumb") {
        settings.scrollbar.horizontal.thumb = std::move(value);
      } else if (path == "scrollbar.horizontal.start") {
        settings.scrollbar.horizontal.start = std::move(value);
      } else if (path == "scrollbar.horizontal.end") {
        settings.scrollbar.horizontal.end = std::move(value);
      } else if (path == "scrollbar.vertical.track") {
        settings.scrollbar.vertical.track = std::move(value);
      } else if (path == "scrollbar.vertical.thumb") {
        settings.scrollbar.vertical.thumb = std::move(value);
      } else if (path == "scrollbar.vertical.start") {
        settings.scrollbar.vertical.start = std::move(value);
      } else if (path == "scrollbar.vertical.end") {
        settings.scrollbar.vertical.end = std::move(value);
      } else if (path == "scrollbar.horizontal.backward") {
        settings.scrollbar.horizontal.backward = std::move(value);
      } else if (path == "scrollbar.horizontal.forward") {
        settings.scrollbar.horizontal.forward = std::move(value);
      } else if (path == "scrollbar.vertical.backward") {
        settings.scrollbar.vertical.backward = std::move(value);
      } else if (path == "scrollbar.vertical.forward") {
        settings.scrollbar.vertical.forward = std::move(value);
      } else if (path == "scrollbar.corner") {
        settings.scrollbar.corner = std::move(value);
      } else if (path == "frame.customTitleDecoration.prefix") {
        settings.frame.customTitleDecoration.prefix = std::move(value);
      } else if (path == "frame.customTitleDecoration.suffix") {
        settings.frame.customTitleDecoration.suffix = std::move(value);
      } else if (path == "frame.customGlyphs.topLeft") {
        settings.frame.customGlyphs.topLeft = std::move(value);
      } else if (path == "frame.customGlyphs.topRight") {
        settings.frame.customGlyphs.topRight = std::move(value);
      } else if (path == "frame.customGlyphs.bottomLeft") {
        settings.frame.customGlyphs.bottomLeft = std::move(value);
      } else if (path == "frame.customGlyphs.bottomRight") {
        settings.frame.customGlyphs.bottomRight = std::move(value);
      } else if (path == "frame.customGlyphs.horizontal") {
        settings.frame.customGlyphs.horizontal = std::move(value);
      } else if (path == "frame.customGlyphs.vertical") {
        settings.frame.customGlyphs.vertical = std::move(value);
      } else if (path == "emptyText") {
        settings.emptyText = std::move(value);
      } else if (path == "unavailableText") {
        settings.unavailableText = std::move(value);
      } else if (path == "entrySeparator") {
        settings.entrySeparator = std::move(value);
      } else if (path == "overflowMarker") {
        settings.overflowMarker = std::move(value);
      } else {
        return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
      }
      return {};
    }

    if (schema->kind == ControlPropertyKind::BOOLEAN) {
      const auto value = name();

      if (value != "true" && value != "false") {
        return std::unexpected(document_.propertyFailure(Code::INVALID_VALUE, offset, type, path));
      }

      const bool enabled = value == "true";

      if (path == "securePolicy.lockPages") {
        settings.securePolicy.lockPages = enabled;
      } else if (path == "securePolicy.excludeFromDumps") {
        settings.securePolicy.excludeFromDumps = enabled;
      } else if (path == "securePolicy.encryptWhileIdle") {
        settings.securePolicy.encryptWhileIdle = enabled;
      } else if (path == "themeCatalog.initialId.enabled") {
        settings.themeCatalog.initialId.enabled = enabled;
      } else if (path == "themeCatalog.layers.enabled") {
        settings.themeCatalog.layers.enabled = enabled;
      } else if (path == "themeSelector.openingShortcut.enabled") {
        settings.themeSelector.openingShortcut.enabled = enabled;
      } else if (path == "notification.refreshDeduplicated") {
        settings.notification.refreshDeduplicated = enabled;
      } else if (path == "notification.reducedMotion") {
        settings.notification.reducedMotion = enabled;
      } else if (path == "message.defaultChoice.enabled") {
        settings.message.defaultChoice.enabled = enabled;
      } else if (path == "message.allowDestructiveDefault") {
        settings.message.allowDestructiveDefault = enabled;
      } else if (path == "active") {
        settings.spinnerActive = enabled;
      } else if (path == "reducedMotion") {
        settings.spinnerReducedMotion = enabled;
      } else if (path == "showStatus") {
        settings.showStatus = enabled;
      } else if (path == "initiallyOpen") {
        settings.initiallyOpen = enabled;
      } else if (path == "followTail") {
        settings.followTail = enabled;
      } else if (path == "scroll") {
        settings.grid.scroll = enabled;
      } else if (path == "required") {
        settings.fieldRequired = enabled;
      } else if (path == "readOnly") {
        settings.readOnly = enabled;
      } else if (path == "focusable") {
        settings.focusable = enabled;
      } else if (path == "frame.enabled") {
        settings.frame.enabled = enabled;
      } else if (path == "frame.sides.top") {
        settings.frame.sides.top = enabled;
      } else if (path == "frame.sides.right") {
        settings.frame.sides.right = enabled;
      } else if (path == "frame.sides.bottom") {
        settings.frame.sides.bottom = enabled;
      } else if (path == "frame.sides.left") {
        settings.frame.sides.left = enabled;
      } else if (path == "arrows") {
        settings.arrows = enabled;
      } else if (path == "scrollbar.ends") {
        settings.scrollbar.ends = enabled;
      } else if (path == "frame.customTitleDecoration.enabled") {
        settings.frame.customTitleDecoration.enabled = enabled;
      } else if (path == "frame.customGlyphs.enabled") {
        settings.frame.customGlyphs.enabled = enabled;
      } else if (path == "yRange.enabled") {
        settings.yRange.enabled = enabled;
      } else if (path == "history.enabled") {
        settings.history.enabled = enabled;
      } else if (path == "history.groupTyping") {
        settings.history.groupTyping = enabled;
      } else if (path == "history.groupDeletion") {
        settings.history.groupDeletion = enabled;
      } else if (path == "dialog.closeButton.enabled") {
        settings.dialog.closeButton.enabled = enabled;
      } else if (path == "dialog.closeButton.focusable") {
        settings.dialog.closeButton.focusable = enabled;
      } else if (path == "dialog.closeOnEscape") {
        settings.dialog.closeOnEscape = enabled;
      } else {
        return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
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

      if (path == "securePolicy.storage") {
        settings.securePolicy.storage = static_cast<SecureStringStorage>(index);
      } else if (path == "pastePolicy") {
        settings.pastePolicy = static_cast<PasswordPastePolicy>(index);
      } else if (path == "preset") {
        settings.spinnerPreset = static_cast<SpinnerPreset>(index);
      } else if (path == "direction") {
        settings.spinnerDirection = static_cast<SpinnerDirection>(index);
      } else if (path == "alignment") {
        settings.spinnerAlignment = static_cast<LayoutAlign>(index);
      } else if (path == "state") {
        settings.state = static_cast<CheckState>(index);
      } else if (path == "cycle") {
        settings.cycle = static_cast<CheckCycle>(index);
      } else if (path == "editMode") {
        settings.editMode = static_cast<EditMode>(index);
      } else if (path == "programmaticChange") {
        settings.programmaticChange = static_cast<ProgrammaticChange>(index);
      } else if (path == "flow") {
        settings.grid.flow = static_cast<GridFlow>(index);
      } else if (path == "mainAlign") {
        settings.linear.mainAlign = static_cast<LayoutAlign>(index);
      } else if (path == "crossAlign") {
        settings.linear.crossAlign = static_cast<LayoutAlign>(index);
      } else if (path == "overflow") {
        settings.linear.overflow = static_cast<LayoutOverflow>(index);
      } else if (path == "chrome") {
        settings.chrome = static_cast<ButtonChrome>(index);
      } else if (path == "glyphs") {
        settings.glyphs = static_cast<PrimitiveGlyphs>(index);
      } else if (path == "glyphProfile") {
        settings.glyphProfile = static_cast<GlyphProfile>(index);
      } else if (path == "glyphFallback") {
        settings.glyphFallback = static_cast<GlyphFallback>(index);
      } else if (path == "frame.glyphs") {
        settings.frame.glyphs = static_cast<PrimitiveGlyphs>(index);
      } else if (path == "frame.pattern") {
        constexpr std::array PATTERNS{FramePattern::SINGLE, FramePattern::SINGLE, FramePattern::ROUNDED,
                                      FramePattern::DOUBLE, FramePattern::HEAVY,  FramePattern::ASCII};

        settings.frame.pattern = PATTERNS[static_cast<std::size_t>(index)];
      } else if (path == "scrollbar.trackPattern") {
        settings.scrollbar.trackPattern = static_cast<ScrollbarPattern>(index);
      } else if (path == "scrollbar.thumbPattern") {
        settings.scrollbar.thumbPattern = static_cast<ScrollbarThumbPattern>(index);
      } else if (path == "scrollbar.arrowPattern") {
        settings.scrollbar.arrowPattern = static_cast<ScrollbarArrowPattern>(index);
      } else if (path == "scrollbar.pattern") {
        settings.scrollbar.pattern = static_cast<ScrollbarPattern>(index);
      } else if (path == "frame.titleDecoration") {
        settings.frame.titleDecoration = static_cast<TitleDecoration>(index);
      } else if (path == "frame.titleAlignment") {
        settings.frame.titleAlignment = static_cast<FrameTitleAlignment>(index);
      } else if (path == "horizontal") {
        settings.horizontal = static_cast<ScrollBarPolicy>(index);
      } else if (path == "vertical") {
        settings.vertical = static_cast<ScrollBarPolicy>(index);
      } else if (path == "placement") {
        settings.placement = static_cast<ScrollPlacement>(index);
      } else if (path == "axis") {
        settings.axis = static_cast<ScrollAxis>(index);
      } else if (path == "horizontalAlign") {
        settings.horizontalAlign = static_cast<LayoutAlign>(index);
      } else if (path == "verticalAlign") {
        settings.verticalAlign = static_cast<LayoutAlign>(index);
      } else if (path == "contentPolicy") {
        settings.contentPolicy = static_cast<DataContentPolicy>(index);
      } else if (path == "mode") {
        settings.mode = static_cast<ChartMode>(index);
      } else if (path == "notification.placement") {
        settings.notification.placement = static_cast<ToastPlacement>(index);
      } else if (path == "notification.overflow") {
        settings.notification.overflow = static_cast<ToastOverflowPolicy>(index);
      } else if (path == "message.buttons") {
        settings.message.buttons = static_cast<DialogButtons>(index);
      } else if (path == "message.order.first") {
        settings.message.order.first = static_cast<MessageChoice>(index);
      } else if (path == "message.order.second") {
        settings.message.order.second = static_cast<MessageChoice>(index);
      } else if (path == "message.order.third") {
        settings.message.order.third = static_cast<MessageChoice>(index);
      } else if (path == "dialog.closeButton.placement") {
        settings.dialog.closeButton.placement = static_cast<DialogClosePlacement>(index);
      } else if (path == "message.defaultChoice.value") {
        settings.message.defaultChoice.value = static_cast<MessageChoice>(index);
      } else if (path == "dialog.mode") {
        settings.dialog.mode = static_cast<DialogMode>(index);
      } else if (path == "dialog.placement") {
        settings.dialog.placement = static_cast<DialogPlacement>(index);
      } else if (path == "dialog.frame.glyphs") {
        settings.dialog.frame.glyphs = static_cast<PrimitiveGlyphs>(index);
      } else if (path == "chartGlyphSet") {
        settings.chartGlyphSet = static_cast<ChartGlyphSet>(index);
      } else if (path == "searchStrategy") {
        settings.searchStrategy = static_cast<SearchStrategy>(index);
      } else if (path == "displayMode") {
        settings.displayMode = static_cast<LineEditDisplayMode>(index);
      } else {
        return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
      }

      return {};
    }

    if (schema->kind == ControlPropertyKind::FLOATING_POINT) {
      auto value = floating();

      if (!value) {
        return std::unexpected(withProperty(value.error(), type, path));
      }

      if (path == "value") {
        settings.value = *value;
      } else if (path == "minimum") {
        settings.minimum = *value;
      } else if (path == "maximum") {
        settings.maximum = *value;
      } else if (path == "yRange.minimum") {
        settings.yRange.minimum = *value;
      } else if (path == "yRange.maximum") {
        settings.yRange.maximum = *value;
      } else if (path == "wheelStep") {
        settings.wheelStep = *value;
      } else {
        return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
      }

      return {};
    }

    if (path == "keyStep" || path == "pageOverlap" || path == "spinnerInterval" || path == "interval") {
      const auto value = capacity<std::uint64_t>();

      if (!value) {
        return std::unexpected(withProperty(value.error(), type, path));
      }

      if (path == "interval") {
        settings.spinnerTick = *value;
      } else if (path == "spinnerInterval") {
        settings.spinnerInterval = *value;
      } else if (path == "keyStep") {
        settings.keyStep = *value;
      } else {
        settings.pageOverlap = *value;
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

    if (path == "maxFrames") {
      settings.spinnerMaxFrames = *value;
    } else if (path == "maxFrameBytes") {
      settings.spinnerMaxFrameBytes = *value;
    } else if (path == "maxDocumentBytes") {
      settings.findMaxDocumentBytes = *value;
    } else if (path == "maxMatches") {
      settings.findMaxMatches = *value;
    } else if (path == "maxFrameCells") {
      settings.spinnerMaxFrameCells = *value;
    } else if (path == "scrollbar.maxGlyphBytes") {
      settings.scrollbar.maxGlyphBytes = *value;
    } else if (path == "securePolicy.maxBytes") {
      settings.securePolicy.maxBytes = *value;
    } else if (path == "maxLayers") {
      settings.maxLayers = *value;
    } else if (path == "dialog.maximumPanelSize.width") {
      settings.dialog.maximumPanelSize.width = static_cast<std::int32_t>(*value);
    } else if (path == "dialog.maximumPanelSize.height") {
      settings.dialog.maximumPanelSize.height = static_cast<std::int32_t>(*value);
    } else if (path == "dialog.viewportMargin") {
      settings.dialog.viewportMargin = static_cast<std::int32_t>(*value);
    } else if (path == "dialog.anchorGap") {
      settings.dialog.anchorGap = static_cast<std::int32_t>(*value);
    } else if (path == "dialog.frame.maxPaintCells") {
      settings.dialog.frame.maxPaintCells = *value;
    } else if (path == "width") {
      settings.width = static_cast<std::int32_t>(*value);
    } else if (path == "height") {
      settings.height = static_cast<std::int32_t>(*value);
    } else if (path == "gap") {
      settings.gap = static_cast<std::int32_t>(*value);
    } else if (path == "flex") {
      settings.flex = static_cast<std::uint32_t>(*value);
    } else if (path == "format.maxBytesPerBinding") {
      settings.format.maxBytesPerBinding = *value;
    } else if (path == "rowHeight") {
      settings.rowHeight = static_cast<std::int32_t>(*value);
    } else if (path == "statusRows") {
      settings.statusRows = static_cast<std::int32_t>(*value);
    } else if (path == "candleGap") {
      settings.candleGap = static_cast<std::int32_t>(*value);
    } else if (path == "overscanRows") {
      settings.overscanRows = *value;
    } else if (path == "maxColumns") {
      settings.maxColumns = *value;
    } else if (path == "maxPreparedCells") {
      settings.maxPreparedCells = *value;
    } else if (path == "maxPreparedTextBytes") {
      settings.maxPreparedTextBytes = *value;
    } else if (path == "maxLines") {
      settings.maxLines = *value;
    } else if (path == "maxBytes") {
      settings.maxBytes = *value;
    } else if (path == "maxBatchBytes") {
      settings.maxBatchBytes = *value;
    } else if (path == "maxRecordBytes") {
      settings.maxRecordBytes = *value;
    } else if (path == "notification.capacity") {
      settings.notification.capacity = *value;
    } else if (path == "notification.maxVisible") {
      settings.notification.maxVisible = *value;
    } else if (path == "notification.maximumWidth") {
      settings.notification.maximumWidth = static_cast<std::int32_t>(*value);
    } else if (path == "notification.margin") {
      settings.notification.margin = static_cast<std::int32_t>(*value);
    } else if (path == "notification.gap") {
      settings.notification.gap = static_cast<std::int32_t>(*value);
    } else if (path == "themeCatalog.maxChoices") {
      settings.themeCatalog.maxChoices = *value;
    } else if (path == "themeCatalog.maxIdBytes") {
      settings.themeCatalog.maxIdBytes = *value;
    } else if (path == "themeCatalog.maxLabelBytes") {
      settings.themeCatalog.maxLabelBytes = *value;
    } else if (path == "themeCatalog.maxDescriptionBytes") {
      settings.themeCatalog.maxDescriptionBytes = *value;
    } else if (path == "themeCatalog.maxTextBytes") {
      settings.themeCatalog.maxTextBytes = *value;
    } else if (path == "themeCatalog.textLimits.maxInputBytes") {
      settings.themeCatalog.textLimits.maxInputBytes = *value;
    } else if (path == "themeCatalog.textLimits.maxClusterBytes") {
      settings.themeCatalog.textLimits.maxClusterBytes = *value;
    } else if (path == "themeCatalog.textLimits.maxGraphemes") {
      settings.themeCatalog.textLimits.maxGraphemes = *value;
    } else if (path == "themeCatalog.textLimits.maxCells") {
      settings.themeCatalog.textLimits.maxCells = *value;
    } else if (path == "themeCatalog.layers.maxLayers") {
      settings.themeCatalog.layers.maxLayers = *value;
    } else if (path == "themeCatalog.layers.maxRules") {
      settings.themeCatalog.layers.maxRules = *value;
    } else if (path == "themeCatalog.layers.maxDeclarations") {
      settings.themeCatalog.layers.maxDeclarations = *value;
    } else if (path == "themeCatalog.layers.maxOwnedBytes") {
      settings.themeCatalog.layers.maxOwnedBytes = *value;
    } else if (path == "themeSelector.textLimits.maxInputBytes") {
      settings.themeSelector.textLimits.maxInputBytes = *value;
    } else if (path == "themeSelector.textLimits.maxClusterBytes") {
      settings.themeSelector.textLimits.maxClusterBytes = *value;
    } else if (path == "themeSelector.textLimits.maxGraphemes") {
      settings.themeSelector.textLimits.maxGraphemes = *value;
    } else if (path == "themeSelector.textLimits.maxCells") {
      settings.themeSelector.textLimits.maxCells = *value;
    } else if (path == "notification.textLimits.maxInputBytes") {
      settings.notification.textLimits.maxInputBytes = *value;
    } else if (path == "notification.textLimits.maxClusterBytes") {
      settings.notification.textLimits.maxClusterBytes = *value;
    } else if (path == "notification.textLimits.maxGraphemes") {
      settings.notification.textLimits.maxGraphemes = *value;
    } else if (path == "notification.textLimits.maxCells") {
      settings.notification.textLimits.maxCells = *value;
    } else if (path == "notification.actionLabelBytes") {
      settings.notification.actionLabelBytes = *value;
    } else if (path == "notification.dedupKeyBytes") {
      settings.notification.dedupKeyBytes = *value;
    } else if (path == "notification.maxPaintCells") {
      settings.notification.maxPaintCells = *value;
    } else if (path == "message.maxBodyLines") {
      settings.message.maxBodyLines = *value;
    } else if (path == "message.textLimits.maxInputBytes") {
      settings.message.textLimits.maxInputBytes = *value;
    } else if (path == "message.textLimits.maxClusterBytes") {
      settings.message.textLimits.maxClusterBytes = *value;
    } else if (path == "message.textLimits.maxGraphemes") {
      settings.message.textLimits.maxGraphemes = *value;
    } else if (path == "message.textLimits.maxCells") {
      settings.message.textLimits.maxCells = *value;
    } else if (path == "history.maxOperations") {
      settings.history.maxOperations = *value;
    } else if (path == "history.maxBytes") {
      settings.history.maxBytes = *value;
    } else if (path == "limits.maxInputBytes") {
      settings.limits.maxInputBytes = *value;
    } else if (path == "limits.maxClusterBytes") {
      settings.limits.maxClusterBytes = *value;
    } else if (path == "limits.maxGraphemes") {
      settings.limits.maxGraphemes = *value;
    } else if (path == "limits.maxCells") {
      settings.limits.maxCells = *value;
    } else if (path == "maxPaintCells") {
      settings.maxPaintCells = *value;
    } else if (path == "preferredWidth") {
      settings.preferredWidth = static_cast<std::int32_t>(*value);
    } else if (path == "arrowDelay") {
      settings.arrowDelay = *value;
    } else if (path == "arrowRepeat") {
      settings.arrowRepeat = *value;
    } else if (path == "minimumThumb") {
      settings.minimumThumb = static_cast<std::int32_t>(*value);
    } else if (path == "frame.maxPaintCells") {
      settings.frame.maxPaintCells = *value;
    } else if (path == "frame.maxTitleBytes") {
      settings.frame.maxTitleBytes = *value;
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
    } else {
      return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, offset, type, path));
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
      const auto key = propertyName();
      const auto canonicalKey = detail::ControlContextAccess::canonicalProperty(key);
      std::string path(0, '\0');

      propertyPath(path, prefix, canonicalKey);

      if (key.empty()) {
        return std::unexpected(document_.propertyFailure(Code::SYNTAX, begin, type, path));
      }

      const bool assigned = take('=') || take(':');

      skip();

      if (path == "frame" && options_.format == SceneResourceFormat::CONFIGURATION && assigned && peek() != '{') {
        auto parsed = compactFrame(settings, type, begin, properties);

        if (!parsed) {
          return parsed;
        }

        continue;
      }

      if (properties.size() == options_.limits.maxProperties) {
        return std::unexpected(document_.propertyFailure(Code::PROPERTY_LIMIT, begin, type, path));
      }

      if (std::ranges::find(properties, canonicalKey) != properties.end()) {
        return std::unexpected(document_.propertyFailure(Code::DUPLICATE_PROPERTY, begin, type, path));
      }

      properties.push_back(canonicalKey);

      if (take('{')) {
        const bool configurationStruct =
          options_.format == SceneResourceFormat::CONFIGURATION &&
          (path == "scrollbar" || path == "scrollbar.horizontal" || path == "scrollbar.vertical" ||
           path == "securePolicy" || path == "limits" || path == "frame" || path == "frame.sides" ||
           path == "frame.customGlyphs" || path == "frame.customTitleDecoration" || path == "format" ||
           path == "yRange" || path == "labels" || path == "dialog" || path == "dialog.closeButton" ||
           path == "dialog.frame" || path == "dialog.maximumPanelSize" || path == "message" ||
           path == "message.order" || path == "message.labels" || path == "message.defaultChoice" ||
           path == "message.textLimits" || path == "notification" || path == "notification.textLimits" ||
           path == "themeSelector" || path == "themeSelector.labels" || path == "themeSelector.textLimits" ||
           path == "themeSelector.openingShortcut" || path == "themeCatalog" || path == "themeCatalog.textLimits" ||
           path == "themeCatalog.initialId" || path == "themeCatalog.layers");

        if (path != "history" && path != "padding" && !configurationStruct) {
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

  // Locate structural braces without decoding or allocating string values. The typed
  // property parser owns string validation and budgets, with the original property path.
  std::expected<void, ManagedError> skipQuotedRange() {
    const auto begin = position_++;

    while (position_ < document_.bytes.size()) {
      const auto value = document_.bytes[position_++];

      if (value == '"') {
        return {};
      }

      if (value == '\\' && position_ < document_.bytes.size()) {
        ++position_;
      }
    }

    return std::unexpected(document_.failure(Code::SYNTAX, begin));
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
          auto parsed = skipQuotedRange();

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

  RegionStatus region(std::size_t parent, GridRegion kind, std::size_t depth, std::size_t offset) {
    const bool declared =
      kind == GridRegion::HEADER ? document_.nodes[parent].headerDeclared : document_.nodes[parent].bodyDeclared;
    if (declared || document_.nodes[parent].directChildren) {
      return parseFailure(Code::INVALID_VALUE, offset);
    }

    if (kind == GridRegion::HEADER) {
      document_.nodes[parent].headerDeclared = true;
    } else {
      document_.nodes[parent].bodyDeclared = true;
    }

    document_.nodes[parent].regions = true;
    if (!take('{')) {
      return parseFailure(Code::SYNTAX, position_);
    }

    bool stickyDeclared{};
    while (!take('}')) {
      skip();
      const auto begin = position_;
      const auto token = name();
      const bool assigned = take(':') || take('=');
      if (assigned) {
        if (kind != GridRegion::HEADER || token != "sticky") {
          return parseFailure(Code::UNKNOWN_PROPERTY, begin, document_.nodes[parent].name, token);
        }

        if (stickyDeclared) {
          return parseFailure(Code::DUPLICATE_PROPERTY, begin, document_.nodes[parent].name, "header.sticky");
        }

        stickyDeclared = true;
        const auto value = name();
        if (value != "true" && value != "false") {
          return parseFailure(Code::INVALID_VALUE, begin, document_.nodes[parent].name, "header.sticky");
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

  bool acceptsNativeProperty(const ResourceNode &target, detail::ControlFactoryKind kind, std::string_view path) const {
    if (options_.format != SceneResourceFormat::CONFIGURATION ||
        (target.factory.kind != kind && !(kind == detail::ControlFactoryKind::THEME_SELECTOR_BINDING &&
                                          target.factory.kind == detail::ControlFactoryKind::THEME_CATALOG_BINDING))) {
      return false;
    }

    // beginNode has already resolved aliases to this concrete factory; caller-owned names never enter this branch.
    const auto &concrete = detail::ResourceAccess::entry(options_.registry, target.factory.index);
    const auto semantic =
      concrete.factoryKind == detail::ControlFactoryKind::MESSAGE_BINDING          ? "MessageBox"
      : concrete.factoryKind == detail::ControlFactoryKind::NOTIFICATION_BINDING   ? "Notifications"
      : concrete.factoryKind == detail::ControlFactoryKind::THEME_CATALOG_BINDING  ? "ThemeSelectorCatalog"
      : concrete.factoryKind == detail::ControlFactoryKind::THEME_SELECTOR_BINDING ? "ThemeSelector"
                                                                                   : std::string_view(concrete.name);
    const auto schema = controlPropertySchema(semantic);

    return std::ranges::any_of(schema, [path](const auto &property) {
      return property.path == path;
    });
  }

  bool acceptsFrameSettings(const ResourceNode &target) const {
    return options_.format == SceneResourceFormat::CONFIGURATION &&
           detail::ResourceAccess::entry(options_.registry, target.factory.index).sized;
  }

  bool acceptsChartRange(const ResourceNode &target) const {
    if (options_.format != SceneResourceFormat::CONFIGURATION) {
      return false;
    }

    const auto &concrete = detail::ResourceAccess::entry(options_.registry, target.factory.index);
    constexpr std::string_view NARROW[]{"text"};

    // Exact code-bound charts opt in through their expanded schema; old explicit allowlists stay narrow.
    return concrete.virtualView == "Chart" && !controls_.matchesRegisteredType(concrete.name, NARROW);
  }

  // Header/property error temporaries must not remain in every bounded recursive node frame.
  std::expected<void, ManagedError> nodeProperty(std::size_t index, std::string_view token, bool positional,
                                                 std::size_t begin, std::vector<std::string_view> &properties) {
    auto &target = document_.nodes[index];
    const auto type = std::string_view(target.name);
    auto key = positional ? std::string_view("text") : token;
    const bool registered = target.factory.kind == detail::ControlFactoryKind::USER ||
                            target.factory.kind == detail::ControlFactoryKind::EXISTING_BUILTIN;
    const bool protectedEditor =
      registered && detail::ResourceAccess::entry(options_.registry, target.factory.index).protectedEditor;

    if (options_.format == SceneResourceFormat::CONFIGURATION || protectedEditor) {
      key = detail::ControlContextAccess::canonicalProperty(key);
    }

    skip();

    if (key == "frame" && options_.format == SceneResourceFormat::CONFIGURATION && peek() != '{') {
      return compactFrame(target.arguments.settings, type, begin, properties);
    }

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

    if (protectedEditor && key == "text") {
      skip();

      // Admit only an exact empty literal, without decoding or retaining a rejected value.
      if (document_.bytes.compare(position_, 2, "\"\"") != 0) {
        return std::unexpected(document_.propertyFailure(Code::SECRET_LITERAL, begin, type, "text"));
      }
    }

    if (protectedEditor && (key == "securePolicy" || key == "limits") && take('{')) {
      return settingsBody(target.arguments.settings, type, key, 2);
    }

    if (protectedEditor && options_.format != SceneResourceFormat::CONFIGURATION && key != "text" && key != "width" &&
        key != "height" && key != "flex" && key != "classes" && key != "visible" && key != "enabled") {
      const auto schema = controlPropertySchema("Password");

      if (std::ranges::find(schema, key, &ControlPropertyDescriptor::path) == schema.end()) {
        return std::unexpected(document_.propertyFailure(Code::UNKNOWN_PROPERTY, begin, type, key));
      }

      return setting(target.arguments.settings, type, key, begin);
    }

    if (key == "help" &&
        acceptsNativeProperty(target, detail::ControlFactoryKind::KEY_HELP_FALLBACK, "entrySeparator")) {
      std::string id;
      auto parsed = string(id);
      if (!parsed) {
        return std::unexpected(withProperty(parsed.error(), type, key));
      }
      target.helpId = std::move(id);
      target.helpOffset = begin;
      return {};
    }

    if (key == "classes") {
      std::string value(0, '\0');
      auto parsed = string(value);

      if (!parsed) {
        return std::unexpected(withProperty(parsed.error(), type, key));
      }

      target.arguments.classes = std::move(value);

      return {};
    }

    if (key == "visible" || key == "enabled") {
      const auto value = name();

      if (value != "true" && value != "false") {
        return std::unexpected(document_.propertyFailure(Code::INVALID_VALUE, begin, type, key));
      }

      auto &flag = key == "visible" ? target.arguments.visible : target.arguments.enabled;

      flag = value == "true";

      return {};
    }

    if ((key == "horizontal" || key == "vertical") &&
        acceptsNativeProperty(target, detail::ControlFactoryKind::SCROLL_FALLBACK, key)) {
      return setting(target.arguments.settings, type, key, begin);
    }

    if (key == "row" || key == "column" || key == "rowSpan" || key == "columnSpan" || key == "horizontal" ||
        key == "vertical") {
      return itemProperty(target, key, begin);
    }

    if (key == "padding" && take('{')) {
      const auto firstOrigin = target.arguments.settings.origins.size();
      auto parsed = settingsBody(target.arguments.settings, type, "padding", 2);

      if (parsed && typed_ && options_.format != SceneResourceFormat::CONFIGURATION &&
          target.factory.kind == detail::ControlFactoryKind::EXISTING_BUILTIN &&
          target.arguments.settings.origins.size() != firstOrigin) {
        const auto &concrete = detail::ResourceAccess::entry(options_.registry, target.factory.index).name;

        if (concrete == "VBox" || concrete == "HBox") {
          const auto &origin = target.arguments.settings.origins[firstOrigin];

          return std::unexpected(
            document_.propertyFailure(Code::UNKNOWN_PROPERTY, origin.source->byteOffset, type, origin.path));
        }
      }

      return parsed;
    }

    if (options_.format != SceneResourceFormat::CONFIGURATION) {
      auto parsed = property(target, key, begin);

      if (parsed && typed_ && target.factory.kind != detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK) {
        target.arguments.settings.origins.push_back(
          {std::string(key == "checked" ? "state" : key), document_.origin(begin)});
      }

      return parsed;
    }

    if ((key == "history" || key == "limits" ||
         (key == "format" &&
          acceptsNativeProperty(target, detail::ControlFactoryKind::KEY_HELP_FALLBACK, "format.maxBytesPerBinding")) ||
         (key == "frame" && acceptsFrameSettings(target)) || (key == "yRange" && acceptsChartRange(target)) ||
         (key == "scrollbar" &&
          acceptsNativeProperty(target, detail::ControlFactoryKind::SCROLL_FALLBACK, "scrollbar.pattern")) ||
         (key == "labels" &&
          acceptsNativeProperty(target, detail::ControlFactoryKind::DATA_STATE_FALLBACK, "labels.loading")) ||
         (key == "themeCatalog" && acceptsNativeProperty(target, detail::ControlFactoryKind::THEME_CATALOG_BINDING,
                                                         "themeCatalog.maxChoices")) ||
         (key == "themeSelector" && acceptsNativeProperty(target, detail::ControlFactoryKind::THEME_SELECTOR_BINDING,
                                                          "themeSelector.labels.apply")) ||
         (key == "notification" &&
          acceptsNativeProperty(target, detail::ControlFactoryKind::NOTIFICATION_BINDING, "notification.capacity")) ||
         (key == "message" && acceptsNativeProperty(target, detail::ControlFactoryKind::MESSAGE_BINDING, "body")) ||
         (key == "dialog" &&
          (acceptsNativeProperty(target, detail::ControlFactoryKind::DIALOG_FALLBACK, "dialog.mode") ||
           acceptsNativeProperty(target, detail::ControlFactoryKind::MESSAGE_BINDING, "dialog.mode") ||
           acceptsNativeProperty(target, detail::ControlFactoryKind::THEME_SELECTOR_BINDING, "dialog.mode")))) &&
        take('{')) {
      return settingsBody(target.arguments.settings, type, key, 2);
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

  // Recursive node/region frames retain only small statuses. The parser owns the
  // first full error until parse() copies it at the public boundary; no heap or
  // caller-owned pointer is required, even when a new typed cause grows.
  std::unexpected<const ManagedError *> parseFailure(const ManagedError &failure) {
    if (!parseFailure_) {
      parseFailure_.emplace(failure);
    }
    return std::unexpected<const ManagedError *>(&*parseFailure_);
  }

  std::unexpected<const ManagedError *> parseFailure(Code code, std::size_t offset) {
    return parseFailure(document_.failure(code, offset));
  }

  std::unexpected<const ManagedError *> parseFailure(Code code, std::size_t offset, std::string_view type,
                                                     std::string_view path) {
    return parseFailure(document_.propertyFailure(code, offset, type, path));
  }

  NodeStatus beginNodeStatus(std::size_t depth) {
    auto result = beginNode(depth);
    if (!result) {
      return parseFailure(result.error());
    }
    return *result;
  }

  RegionStatus nodePropertyStatus(std::size_t index, std::string_view token, bool positional, std::size_t begin,
                                  std::vector<std::string_view> &properties) {
    auto result = nodeProperty(index, token, positional, begin, properties);
    if (!result) {
      return parseFailure(result.error());
    }
    return {};
  }

  NodeStatus node(std::size_t depth) {
    auto begun = beginNodeStatus(depth);

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
        return parseFailure(Code::SYNTAX, position_);
      }

      const auto begin = position_;
      const auto token = peek() == '"' ? std::string_view{} : propertyName();
      const bool named = take(':') || take('=');
      const bool positional = !named && (token.empty() || token == "i18n");
      // A caller-owned child name wins the bare spelling; frame={...} remains an explicit native property.
      const bool frameGroup = token == "frame" && acceptsFrameSettings(document_.nodes[index]) &&
                              !detail::ResourceAccess::find(options_.registry, token);
      const bool scrollbarGroup =
        token == "scrollbar" &&
        acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::SCROLL_FALLBACK,
                              "scrollbar.pattern") &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool formatGroup =
        token == "format" &&
        acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::KEY_HELP_FALLBACK,
                              "format.maxBytesPerBinding") &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool chartRangeGroup = token == "yRange" && acceptsChartRange(document_.nodes[index]) &&
                                   !detail::ResourceAccess::find(options_.registry, token) &&
                                   !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool labelsGroup =
        token == "labels" &&
        acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::DATA_STATE_FALLBACK,
                              "labels.loading") &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool dialogGroup =
        token == "dialog" &&
        (acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::DIALOG_FALLBACK, "dialog.mode") ||
         acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::MESSAGE_BINDING, "dialog.mode") ||
         acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::THEME_SELECTOR_BINDING,
                               "dialog.mode")) &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool messageGroup =
        token == "message" &&
        acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::MESSAGE_BINDING, "body") &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool catalogGroup =
        token == "themeCatalog" &&
        acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::THEME_CATALOG_BINDING,
                              "themeCatalog.maxChoices") &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool selectorGroup =
        token == "themeSelector" &&
        acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::THEME_SELECTOR_BINDING,
                              "themeSelector.labels.apply") &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool notificationGroup =
        token == "notification" &&
        acceptsNativeProperty(document_.nodes[index], detail::ControlFactoryKind::NOTIFICATION_BINDING,
                              "notification.capacity") &&
        !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool overlayPaddingChild =
        token == "padding" && document_.nodes[index].factory.kind == detail::ControlFactoryKind::OVERLAY_FALLBACK &&
        (detail::ResourceAccess::find(options_.registry, token) ||
         detail::ControlContextAccess::hasDeclaredType(controls_, token));
      const auto kind = document_.nodes[index].factory.kind;
      const bool protectedGroup =
        (kind == detail::ControlFactoryKind::USER || kind == detail::ControlFactoryKind::EXISTING_BUILTIN) &&
        detail::ResourceAccess::entry(options_.registry, document_.nodes[index].factory.index).protectedEditor &&
        (token == "securePolicy" || token == "limits") && !detail::ResourceAccess::find(options_.registry, token) &&
        !detail::ControlContextAccess::hasDeclaredType(controls_, token);
      const bool structured =
        (protectedGroup && peek() == '{') ||
        (options_.format == SceneResourceFormat::CONFIGURATION &&
         (token == "history" || token == "limits" || (token == "padding" && !overlayPaddingChild) || frameGroup ||
          scrollbarGroup || formatGroup || chartRangeGroup || labelsGroup || dialogGroup || messageGroup ||
          notificationGroup || selectorGroup || catalogGroup) &&
         peek() == '{') ||
        (document_.nodes[index].factory.kind == detail::ControlFactoryKind::DECLARATIVE_GRID_FALLBACK &&
         token == "padding" && peek() == '{');

      if (named || positional || structured) {
        auto applied = nodePropertyStatus(index, token, positional, begin, properties);

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
          return parseFailure(Code::INVALID_VALUE, begin);
        }
        if (!detail::ResourceAccess::entry(options_.registry, document_.nodes[index].type).children &&
            document_.nodes[index].factory.kind != detail::ControlFactoryKind::MESSAGE_BINDING) {
          return parseFailure(Code::CHILDREN_FORBIDDEN, begin);
        }
        document_.nodes[index].directChildren = true;

        position_ = begin;

        auto child = node(depth + 1);

        if (!child) {
          return std::unexpected(child.error());
        }

        document_.nodes[*child].parent = index;
        ++document_.nodes[index].directChildCount;
      }
    }
  }

  std::optional<ManagedError> parseFailure_{};
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

} // namespace

std::shared_ptr<const ControlSourceOrigin> ResourceDocument::origin(std::size_t offset) const {
  ControlSourceOrigin result;

  result.sharedSourceIdentity = identity;
  result.byteOffset = offset;

  const auto line = std::ranges::upper_bound(lines, offset);

  result.line = static_cast<std::size_t>(line - lines.begin());
  result.column = offset - *(line - 1) + 1;

  return std::make_shared<const ControlSourceOrigin>(std::move(result));
}

ManagedError ResourceDocument::failure(Code code, std::size_t offset, std::error_code system, bool positioned) const {
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

ManagedError ResourceDocument::propertyFailure(Code code, std::size_t offset, std::string_view type,
                                               std::string_view path) const {
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

// Keep borrowed parser statuses inside this call; parse() returns an owning error before destruction.
std::expected<void, ManagedError> parseResourceDocument(ResourceDocument &document, SceneResourceOptions &options,
                                                        ControlContext &controls, bool typed) {
  return Parser(document, options, controls, typed).parse();
}
} // namespace portals::ui::detail::resource
