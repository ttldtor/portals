// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_scene_resource.hpp>

#include <array>

/// @brief Compile-time descriptions translated into the existing owning UI recipes at runtime.
namespace portals::ui::ct {
/** @brief Owning fixed-size bytes usable as a structural template argument.
 * @tparam N Array extent including its terminal null byte; embedded nulls are preserved.
 * UTF-8/grapheme validity is deliberately checked by the existing runtime Unicode core.
 */
template <std::size_t N> struct Literal {
  std::array<char, N> bytes{}; ///< Owned bytes, including the terminator.

  /** @param value Constant array copied in full; its last byte must be null.
   */
  consteval Literal(const char (&value)[N]) {
    static_assert(N > 0, "Static scene text requires a terminated array");

    std::copy_n(value, N, bytes.begin());

    if (bytes.back() != '\0') {
      throw "Static scene text requires a terminated array";
    }
  }

  /** @return Byte view borrowing this value, excluding only the final terminator. */
  constexpr std::string_view view() const noexcept {
    return {bytes.data(), N - 1};
  }
};

namespace detail {
// MSVC 19.44 cannot materialize an immediate class result containing optional's union.
// Keep its value and presence separately until the runtime-facing style is requested.
/** @brief Union-free owning snapshot of one compile-time cell appearance. */
struct PackedCellStyle {
  RgbColor foreground; ///< Foreground value; ignored when absent.
  RgbColor background; ///< Background value; ignored when absent.
  bool hasForeground;  ///< Whether the foreground is an explicit RGB value.
  bool hasBackground;  ///< Whether the background is an explicit RGB value.
  bool bold;           ///< Explicit bold appearance.
  bool italic;         ///< Explicit italic appearance.
  bool underline;      ///< Explicit underline appearance.

  /** @param style Owning appearance whose RGB presence and attributes are preserved. */
  constexpr explicit PackedCellStyle(CellStyle style)
      : foreground(style.foreground.value_or(RgbColor{})), background(style.background.value_or(RgbColor{})),
        hasForeground(style.foreground.has_value()), hasBackground(style.background.has_value()), bold(style.bold),
        italic(style.italic), underline(style.underline) {
  }

  /** @return Original owning appearance, including absent colors and explicit black. */
  constexpr CellStyle value() const {
    CellStyle result{.foreground = {}, .background = {}, .bold = bold, .italic = italic, .underline = underline};

    if (hasForeground) {
      result.foreground = foreground;
    }

    if (hasBackground) {
      result.background = background;
    }

    return result;
  }
};

/** @brief Union-free snapshot preserving each manual control appearance. */
struct PackedControlStyle {
  PackedCellStyle normal;   ///< Ordinary appearance.
  PackedCellStyle focused;  ///< Focused appearance.
  PackedCellStyle disabled; ///< Disabled appearance.

  /** @param style Owning manual appearances copied without optional union storage. */
  constexpr explicit PackedControlStyle(ControlStyle style)
      : normal(style.normal), focused(style.focused), disabled(style.disabled) {
  }

  /** @return Owning manual appearances in their original public representation. */
  constexpr ControlStyle value() const {
    return {normal.value(), focused.value(), disabled.value()};
  }
};

// Named non-constexpr boundaries give both MSVC and GCC useful rejection diagnostics.
/** @brief Compile-time rejection marker for invalid linear settings; never called at runtime. */
void staticSceneInvalidLinearOptions();
/** @brief Compile-time rejection marker for invalid size bounds; never called at runtime. */
void staticSceneInvalidSizePolicy();
/** @brief Compile-time rejection marker for an unknown editor mode; never called at runtime. */
void staticSceneInvalidEditMode();

/** @param axis Existing per-axis constraints.
 * @return True for the same values admitted by the linear layout engine.
 */
constexpr bool validAxis(AxisSize axis) {
  return axis.minimum >= 0 && axis.maximum >= axis.minimum && axis.preferred >= INTRINSIC_EXTENT && axis.weight > 0 &&
         (axis.mode == SizeMode::AUTOMATIC || axis.mode == SizeMode::FIXED || axis.mode == SizeMode::FLEX);
}

/** @param alignment Existing linear alignment value.
 * @return True for a known layout alignment.
 */
constexpr bool validAlignment(LayoutAlign alignment) {
  return alignment == LayoutAlign::START || alignment == LayoutAlign::CENTER || alignment == LayoutAlign::END ||
         alignment == LayoutAlign::STRETCH;
}

/** @param options Existing owning linear settings.
 * @return True for the same values admitted by the linear layout engine.
 */
constexpr bool validLinear(LinearOptions options) {
  return options.gap >= 0 && options.padding.left >= 0 && options.padding.top >= 0 && options.padding.right >= 0 &&
         options.padding.bottom >= 0 && validAlignment(options.main_align) && validAlignment(options.cross_align) &&
         options.overflow == LayoutOverflow::CLIP;
}

struct Access;
} // namespace detail

/** @brief Immutable-through-accessors static node; no live widgets, models, slots or callbacks.
 * @tparam T Existing Label, Button, LineEdit, Row or Column widget type.
 * @tparam Id Optional owning style/adapter identity; nonempty identities must be unique in a scene.
 * @tparam N Owned caption array extent including the terminator; containers use one empty byte.
 * @tparam Children Static Element types stored by value in insertion order.
 * Construct only through the consteval factories. Copies own all text and child storage.
 */
template <class T, Literal Id, std::size_t N, class... Children> class Element {
public:
  using WidgetType = T;          ///< Existing widget type instantiated only at runtime mount.
  static constexpr auto ID = Id; ///< Owning static identity; empty means unnamed.
  static constexpr std::size_t NODE_COUNT = 1 + (Children::NODE_COUNT + ... + 0); ///< Complete subtree node count.

  /** @return Byte view borrowing this node; UTF-8 conversion is deferred to mount. */
  constexpr std::string_view text() const noexcept {
    return text_.view();
  }

  /** @return Owning manual appearances copied into the runtime recipe. */
  constexpr ControlStyle style() const {
    return style_.value();
  }

  /** @return Validated owning size constraints, in terminal cells. */
  constexpr SizePolicy sizePolicy() const {
    return policy_;
  }

  /** @return Validated linear options; relevant only for Row and Column. */
  constexpr LinearOptions linearOptions() const {
    return options_;
  }

  /** @return Existing editor confirmation policy; relevant only for LineEdit. */
  constexpr EditMode editMode() const {
    return mode_;
  }

  /** @return Children borrowed from this value, in the runtime mount order. */
  constexpr const std::tuple<Children...> &children() const noexcept {
    return children_;
  }

private:
  friend struct detail::Access;

  consteval Element(Literal<N> text, ControlStyle style, SizePolicy policy, LinearOptions options, EditMode mode,
                    Children... children)
      : text_(text), style_(style), policy_(policy), options_(options), mode_(mode), children_(children...) {
    if (!detail::validAxis(policy.width) || !detail::validAxis(policy.height)) {
      detail::staticSceneInvalidSizePolicy();
    }

    if (!detail::validLinear(options)) {
      detail::staticSceneInvalidLinearOptions();
    }

    if (mode != EditMode::IMMEDIATE && mode != EditMode::ON_COMPLETION && mode != EditMode::EXPLICIT) {
      detail::staticSceneInvalidEditMode();
    }
  }

  Literal<N> text_;
  detail::PackedControlStyle style_;
  SizePolicy policy_;
  LinearOptions options_;
  EditMode mode_;
  std::tuple<Children...> children_;
};

namespace detail {
/** @brief Recognize exact factory-produced static elements.
 * @tparam T Candidate type after cv/ref normalization.
 */
template <class T> struct IsElement : std::false_type {};

/** @brief Recognize a static node specialization.
 * @tparam T Existing widget type.
 * @tparam Id Optional owning identity.
 * @tparam N Caption extent.
 * @tparam Children Static child types.
 */
template <class T, Literal Id, std::size_t N, class... Children>
struct IsElement<Element<T, Id, N, Children...>> : std::true_type {};

/** @brief Admit only static elements produced by this header's factories.
 * @tparam T Candidate node type.
 */
template <class T>
concept StaticElement = IsElement<std::remove_cvref_t<T>>::value;

/** @tparam Node Static subtree type.
 * @return All owning type-level identities in preorder; views refer to template static storage.
 */
template <StaticElement Node> consteval auto identities() {
  std::array<std::string_view, Node::NODE_COUNT> result{};
  std::size_t index{};

  result[index++] = Node::ID.view();
  if constexpr (Node::NODE_COUNT > 1) {
    []<class... Children>(std::tuple<Children...> *, auto &values, auto &offset) {
      (
        [&] {
          for (auto id : identities<Children>()) {
            values[offset++] = id;
          }
        }(),
        ...);
    }(static_cast<std::remove_cvref_t<decltype(std::declval<Node>().children())> *>(nullptr), result, index);
  }

  return result;
}

/** @tparam Node Static subtree type.
 * @return True when no nonempty identity appears twice anywhere in the subtree.
 */
template <StaticElement Node> consteval bool uniqueIds() {
  constexpr auto IDS = identities<Node>();

  for (std::size_t i = 0; i < IDS.size(); ++i) {
    for (std::size_t j = i + 1; j < IDS.size(); ++j) {
      if (!IDS[i].empty() && IDS[i] == IDS[j]) {
        return false;
      }
    }
  }

  return true;
}

/** @tparam Id Required nonempty identity.
 * @tparam Node Static subtree type.
 * @return Number of matching nodes; zero identifies a miss before any runtime action is stored.
 */
template <Literal Id, StaticElement Node> consteval std::size_t idCount() {
  constexpr auto IDS = identities<Node>();

  return Id.view().empty() ? 0 : std::ranges::count(IDS, Id.view());
}
} // namespace detail

/** @brief Validated static scene value; runtime recipes and mounted ownership remain separate.
 * @tparam Root Factory-produced static root type with unique nonempty identities.
 */
template <detail::StaticElement Root> class StaticScene {
public:
  /** @return Borrowed immutable root whose complete text/children storage belongs to this value. */
  constexpr const Root &root() const noexcept {
    return root_;
  }

private:
  friend struct detail::Access;

  consteval explicit StaticScene(Root root) : root_(root) {
    static_assert(detail::uniqueIds<Root>(), "Static scene duplicate nonempty ID");
  }

  Root root_;
};

namespace detail {
/** @brief Internal construction bridge; factory values cannot bypass constant validation. */
struct Access {
  /** @tparam T Existing supported widget type.
   * @tparam Id Optional static identity.
   * @tparam N Caption extent including the terminator.
   * @tparam Children Static node types.
   * @param text Owned bytes copied from the caller.
   * @param style Owning manual appearances.
   * @param policy Owning cell constraints.
   * @param options Linear layout settings.
   * @param mode Editor confirmation policy.
   * @param children Owning children in insertion order.
   * @return Constant-validated static node.
   */
  template <class T, Literal Id, std::size_t N, StaticElement... Children>
  static consteval auto element(const char (&text)[N], ControlStyle style, SizePolicy policy, LinearOptions options,
                                EditMode mode, Children... children) {
    return Element<T, Id, N, Children...>(Literal(text), style, policy, options, mode, children...);
  }

  /** @tparam Root Factory-produced root type.
   * @param root Static tree stored by value, with no borrowed text.
   * @return Scene whose nonempty identities were validated across the complete subtree.
   */
  template <StaticElement Root> static consteval auto scene(Root root) {
    return StaticScene<Root>(root);
  }
};
} // namespace detail

/** @tparam Id Optional style/adapter identity, unique when nonempty.
 * @tparam N Caption array extent including its terminator.
 * @param text Constant bytes copied into the result; Unicode validity is checked at runtime mount.
 * @param style Owning manual appearances.
 * @param policy Owning validated constraints, in terminal cells.
 * @return A constant-built Label description with owning bytes.
 */
template <Literal Id = "", std::size_t N>
consteval auto label(const char (&text)[N], ControlStyle style = ControlStyle{}, SizePolicy policy = SizePolicy{}) {
  return detail::Access::element<Label, Id>(text, style, policy, {}, EditMode::IMMEDIATE);
}

/** @tparam Id Optional style/adapter identity, unique when nonempty.
 * @tparam N Caption array extent including its terminator.
 * @param text Constant caption bytes copied now, Unicode-validated at runtime mount.
 * @param style Owning manual appearances.
 * @param policy Owning validated constraints, in terminal cells.
 * @return Constant-built unbound Button; add a runtime onClick adapter when needed.
 */
template <Literal Id = "", std::size_t N>
consteval auto button(const char (&text)[N], ControlStyle style = ControlStyle{}, SizePolicy policy = SizePolicy{}) {
  return detail::Access::element<Button, Id>(text, style, policy, {}, EditMode::IMMEDIATE);
}

/** @tparam Id Optional style/adapter identity, unique when nonempty.
 * @tparam N Initial text extent including its terminator.
 * @param initial Constant initial bytes; ignored when an explicit runtime state binding is supplied.
 * @param mode Existing confirmation policy; IMMEDIATE preserves the dynamic factory default.
 * @param style Owning manual appearances.
 * @param policy Owning validated constraints, in terminal cells.
 * @return Constant-built editor description; other editor options use existing runtime factories.
 */
template <Literal Id = "", std::size_t N>
consteval auto lineEdit(const char (&initial)[N], EditMode mode = EditMode::IMMEDIATE,
                        ControlStyle style = ControlStyle{}, SizePolicy policy = SizePolicy{}) {
  return detail::Access::element<LineEdit, Id>(initial, style, policy, {}, mode);
}

/** @tparam Id Optional style/adapter identity, unique when nonempty.
 * @tparam Children Factory-produced static node types.
 * @param options Existing validated horizontal layout settings.
 * @param children Owning static children, mounted left to right.
 * @return Constant-built Row description.
 */
template <Literal Id = "", detail::StaticElement... Children>
consteval auto row(LinearOptions options, Children... children) {
  return detail::Access::element<Row, Id>("", {}, {}, options, EditMode::IMMEDIATE, children...);
}

/** @tparam Id Optional style/adapter identity, unique when nonempty.
 * @tparam Children Factory-produced static node types.
 * @param children Owning static children in insertion order.
 * @return Constant-built Row using existing default linear settings.
 */
template <Literal Id = "", detail::StaticElement... Children> consteval auto row(Children... children) {
  return row<Id>(LinearOptions{}, children...);
}

/** @tparam Id Optional style/adapter identity, unique when nonempty.
 * @tparam Children Factory-produced static node types.
 * @param options Existing validated vertical layout settings.
 * @param children Owning static children, mounted top to bottom.
 * @return Constant-built Column description.
 */
template <Literal Id = "", detail::StaticElement... Children>
consteval auto column(LinearOptions options, Children... children) {
  return detail::Access::element<Column, Id>("", {}, {}, options, EditMode::IMMEDIATE, children...);
}

/** @tparam Id Optional style/adapter identity, unique when nonempty.
 * @tparam Children Factory-produced static node types.
 * @param children Owning static children in insertion order.
 * @return Constant-built Column using existing default linear settings.
 */
template <Literal Id = "", detail::StaticElement... Children> consteval auto column(Children... children) {
  return column<Id>(LinearOptions{}, children...);
}

/** @tparam Root Factory-produced static node type.
 * @param root Owning static tree; duplicate nonempty IDs are compilation errors.
 * @return Validated constant-built scene ready for runtime describe().
 */
template <detail::StaticElement Root> consteval auto scene(Root root) {
  return detail::Access::scene(root);
}

namespace detail {
/** @brief Separate adapter kinds permit one reference, callback and state per identity. */
enum class AdapterKind {
  REFERENCE,   ///< Observing slot publication.
  CLICK,       ///< Scene-owned activation callback.
  TEXT_BINDING ///< Existing scene-owned editor state observation.
};

/** @brief Runtime reference storage, excluded from constant construction.
 * @tparam Id Required nonempty identity.
 * @tparam T Observed existing widget type.
 */
template <Literal Id, class T> struct Reference {
  static constexpr auto ID = Id;                       ///< Required static target identity.
  static constexpr auto KIND = AdapterKind::REFERENCE; ///< Duplicate checking category.
  ui::Ref<T> value;                                    ///< Shared observing slot; owns neither widget nor scene.

  /** @tparam U Existing widget type at the matching identity.
   * @param recipe Consuming existing runtime recipe.
   * @return Recipe with deferred transactional publication; mismatch fails compilation.
   */
  template <class U> auto apply(Description<U> recipe) {
    static_assert(std::same_as<T, U>, "Static scene reference widget type mismatch");

    if constexpr (std::same_as<T, U>) {
      return std::move(recipe).reference(std::move(value));
    }
  }
};

/** @brief Runtime callback storage; captures transfer to the existing managed binding.
 * @tparam Id Required nonempty identity.
 * @tparam Callback Existing compact ManagedAction callable.
 */
template <Literal Id, class Callback> struct Click {
  static constexpr auto ID = Id;                   ///< Required static target identity.
  static constexpr auto KIND = AdapterKind::CLICK; ///< Duplicate checking category.
  Callback value; ///< Owning activation action, never evaluated during constant construction.

  /** @tparam T Existing widget type at the matching identity.
   * @param recipe Consuming existing runtime recipe.
   * @return Recipe with the existing onClick setup; a non-Button target fails compilation.
   */
  template <class T> auto apply(Description<T> recipe) {
    static_assert(std::same_as<T, Button>, "Static scene onClick requires Button");

    if constexpr (std::same_as<T, Button>) {
      return std::move(recipe).onClick(std::move(value));
    }
  }
};

/** @brief Runtime model observation; no alternate property owner or reactive system.
 * @tparam Id Required nonempty editor identity.
 */
template <Literal Id> struct TextBinding {
  static constexpr auto ID = Id;                          ///< Required static target identity.
  static constexpr auto KIND = AdapterKind::TEXT_BINDING; ///< Duplicate checking category.
  ManagedState<std::string> value;                        ///< Observation belonging to the receiving ManagedScene.
};

/** @brief Admit only this header's explicit runtime adapter values.
 * @tparam T Candidate adapter type.
 */
template <class T> struct IsAdapter : std::false_type {};

/** @tparam Id Required identity.
 * @tparam T Observed widget type.
 */
template <Literal Id, class T> struct IsAdapter<Reference<Id, T>> : std::true_type {};

/** @tparam Id Required identity.
 * @tparam Callback Owning compact action.
 */
template <Literal Id, class Callback> struct IsAdapter<Click<Id, Callback>> : std::true_type {};

/** @tparam Id Required editor identity. */
template <Literal Id> struct IsAdapter<TextBinding<Id>> : std::true_type {};

/** @brief Admit exact factory-produced runtime adapter types.
 * @tparam T Candidate runtime type.
 */
template <class T>
concept RuntimeAdapter = IsAdapter<T>::value;

/** @tparam Adapters Factory-produced runtime types.
 * @return True when no adapter kind/identity pair is repeated.
 */
template <RuntimeAdapter... Adapters> consteval bool uniqueAdapters() {
  constexpr std::array<std::string_view, sizeof...(Adapters)> IDS{Adapters::ID.view()...};
  constexpr std::array<AdapterKind, sizeof...(Adapters)> KINDS{Adapters::KIND...};

  for (std::size_t i = 0; i < IDS.size(); ++i) {
    for (std::size_t j = i + 1; j < IDS.size(); ++j) {
      if (IDS[i] == IDS[j] && KINDS[i] == KINDS[j]) {
        return false;
      }
    }
  }

  return true;
}

/** @tparam Node Static node type.
 * @tparam Adapters Runtime adapter types already validated against the complete scene.
 * @param node Static owning description borrowed only while copying runtime recipe storage.
 * @param adapters Owning adapter pack, transferred only at each matching node.
 * @return Existing consuming recipe; construction can throw before mount, as dynamic factories do.
 */
template <StaticElement Node, RuntimeAdapter... Adapters> auto describeNode(const Node &node, Adapters &...adapters) {
  using T = typename Node::WidgetType;

  auto recipe = [&]() -> Description<T> {
    if constexpr (std::same_as<T, Row> || std::same_as<T, Column>) {
      return std::apply(
        [&](const auto &...children) {
          return ui::linear<T>(node.linearOptions(), describeNode(children, adapters...)...);
        },
        node.children());
    } else if constexpr (std::same_as<T, LineEdit>) {
      LineEditConfig config;
      std::optional<ManagedState<std::string>> state;

      config.options.editMode = node.editMode();
      config.options.style = node.style();
      config.policy = node.sizePolicy();
      (
        [&] {
          if constexpr (Adapters::KIND == AdapterKind::TEXT_BINDING && Adapters::ID.view() == Node::ID.view()) {
            state = adapters.value;
          }
        }(),
        ...);

      return state ? ui::lineEdit(*state, std::move(config)) : ui::lineEdit(node.text(), std::move(config));
    } else {
      return ui::control<T>(node.text(), node.style(), node.sizePolicy());
    }
  }();

  if constexpr (!Node::ID.view().empty()) {
    recipe = std::move(recipe).id(Node::ID.view());
  }

  (
    [&] {
      if constexpr (Adapters::ID.view() == Node::ID.view()) {
        if constexpr (Adapters::KIND == AdapterKind::TEXT_BINDING) {
          static_assert(std::same_as<T, LineEdit>, "Static scene bind requires LineEdit");
        } else {
          recipe = adapters.apply(std::move(recipe));
        }
      }
    }(),
    ...);

  return recipe;
}
} // namespace detail

/** @tparam Id Required nonempty static identity.
 * @tparam T Existing widget type, checked against the target during describe().
 * @param value Shared UI-thread reference slot; empty until the complete runtime mount succeeds.
 * @return Runtime adapter owning a slot copy; no scene or widget ownership is retained.
 */
template <Literal Id, class T> auto reference(ui::Ref<T> value) {
  return detail::Reference<Id, T>{std::move(value)};
}

/** @tparam Id Required nonempty Button identity, checked during describe().
 * @tparam Callback Existing void/expected/EditorCommandResult action, optionally borrowing ClickEvent.
 * @param value Owning action; explicit borrowed services must outlive invocation.
 * @return Runtime adapter; captures transfer to ManagedScene at mount, with existing teardown semantics.
 */
template <Literal Id, class Callback>
  requires portals::detail::ManagedAction<Callback, ClickEvent>
auto onClick(Callback value) {
  return detail::Click<Id, Callback>{std::move(value)};
}

/** @tparam Id Required nonempty LineEdit identity, checked during describe().
 * @param value Existing scene-owned text observation; must belong to the receiving ManagedScene.
 * @return Runtime adapter; the model supplies initial text instead of the descriptor's initial bytes.
 */
template <Literal Id> auto bind(ManagedState<std::string> value) {
  return detail::TextBinding<Id>{value};
}

/** @tparam Root Validated static root type.
 * @tparam Adapters Explicit runtime adapter types; IDs and target types are compile-time checked.
 * @param value Owning static scene borrowed only while constructing the returned runtime recipe.
 * @param adapters Owning runtime values consumed into existing recipes; duplicate kind/ID pairs are errors.
 * @return Existing move-only Description suitable for ui::mount or ui::scene; allocation/capture moves may throw
 * before those checked boundaries, just as with dynamic factories. Mount preserves transactional refs,
 * subtree rollback, Unicode errors, UI affinity and scene-owned callback cleanup. No static storage is borrowed
 * by the returned recipe and no second retained tree survives mount.
 */
template <detail::StaticElement Root, detail::RuntimeAdapter... Adapters>
auto describe(const StaticScene<Root> &value, Adapters... adapters) {
  static_assert(((detail::idCount<Adapters::ID, Root>() == 1) && ...),
                "Static scene runtime adapter requires a known nonempty ID");
  static_assert(detail::uniqueAdapters<Adapters...>(), "Static scene duplicate runtime adapter kind and ID");

  return detail::describeNode(value.root(), adapters...);
}
} // namespace portals::ui::ct
