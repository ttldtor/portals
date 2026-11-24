// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_model.hpp>

#include <tuple>

namespace portals::detail {
/// @cond INTERNAL
template <class T> inline constexpr bool IS_LIVE_TEXT_SOURCE<ui::Model<T>> = true;
template <class T> inline constexpr bool IS_LIVE_TEXT_SOURCE<ui::ModelValue<T>> = true;
template <class T, class Member> inline constexpr bool IS_LIVE_TEXT_SOURCE<ui::ModelField<T, Member>> = true;
/// @endcond
} // namespace portals::detail

namespace portals::ui {
namespace detail {
/// @cond INTERNAL
struct ValueTextAccess;

struct ValueTextDeclaration {
  using Create = std::function<std::expected<ManagedState<std::string>, ManagedError>(
    ManagedScene &, Node<Widget>, MountContext &, const std::shared_ptr<Access::ValueCaptionGraph> &)>;
  Create create;
};

/// @endcond
} // namespace detail

/** @brief Owning cold formatted text with explicit scalar dependencies, separate from its input roots.
 * Each receiving caption owns one bounded output and one existing observation per distinct input root.
 * Copies share expression identity; replacing a computed caption's source replaces this entire expression.
 * Formatters and value copies follow Property/derive's pure, non-reentrant computation contract.
 * Hidden recipients update only when dependencies change; no timer or polling is installed.
 */
class ValueText {
public:
  /** @brief Create an empty descriptor whose checked use reports INVALID_ARGUMENT. */
  ValueText() = default;

private:
  friend struct detail::ValueTextAccess;
  std::shared_ptr<detail::ValueTextDeclaration> data_;
};

namespace detail {
/// @cond INTERNAL
template <class T> struct TextArgument {
  using Value = std::conditional_t<std::same_as<std::remove_cvref_t<T>, std::string_view> ||
                                     std::is_array_v<std::remove_reference_t<T>>,
                                   std::string, std::remove_cvref_t<T>>;
  static constexpr bool LIVE = false;
  static constexpr bool SUPPORTED = true;
};

template <class T> struct TextArgument<Model<T>> {
  using Value = T;
  static constexpr bool LIVE = true;
  static constexpr bool SUPPORTED = std::equality_comparable<T> && std::copy_constructible<T>;
};

template <class T> struct TextArgument<ModelValue<T>> : TextArgument<Model<T>> {};

template <class T, class Member> struct TextArgument<ModelField<T, Member>> {
  using Value = std::remove_cv_t<Member>;
  static constexpr bool LIVE = true;
  static constexpr bool SUPPORTED = std::equality_comparable<T> && std::copy_constructible<T>;
};

template <class T> using TextArgumentValue = typename TextArgument<std::remove_cvref_t<T>>::Value;

template <class T>
using OwnedTextArgument =
  std::conditional_t<TextArgument<std::remove_cvref_t<T>>::LIVE, std::remove_cvref_t<T>, TextArgumentValue<T>>;

template <class T>
concept TextFormatArgument =
  TextArgument<std::remove_cvref_t<T>>::SUPPORTED && std::formattable<TextArgumentValue<T>, char> &&
  std::copy_constructible<OwnedTextArgument<T>> && std::constructible_from<OwnedTextArgument<T>, T> &&
  (!std::is_pointer_v<std::remove_cvref_t<T>>) &&
  (!std::is_array_v<std::remove_reference_t<T>> ||
   (std::extent_v<std::remove_reference_t<T>> > 0 &&
    std::same_as<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<T>>>, char>)) &&
  (!std::ranges::view<std::remove_cvref_t<T>> || std::same_as<std::remove_cvref_t<T>, std::string_view>) &&
  (!portals::detail::IS_BORROWED_DERIVED_VALUE<std::remove_cvref_t<T>> ||
   std::same_as<std::remove_cvref_t<T>, std::string_view>);

template <class T> decltype(auto) retainedTextArgument(T &&value) {
  if constexpr (std::is_array_v<std::remove_reference_t<T>>) {
    constexpr auto EXTENT = std::extent_v<std::remove_reference_t<T>>;
    const auto count = EXTENT - (value[EXTENT - 1] == '\0' ? 1 : 0);

    return std::string_view(value, count);
  } else {
    return std::forward<T>(value);
  }
}

template <class Value> struct ValueTextInput {
  using Read = std::expected<std::shared_ptr<const PropertyChange<Value>>, TreeError>;
  using Callback = std::function<std::expected<void, TreeError>()>;
  std::shared_ptr<void> root;
  std::function<Read()> read;
  std::function<std::expected<void, ManagedError>(Callback, const std::shared_ptr<Access::ValueCaptionGraph> &)> attach;
};

template <class Resolver, class... Values>
concept ValueResourceResolver = requires(Resolver &resolver, std::string_view key, const Values &...values) {
  { std::invoke(resolver, key, values...) } -> std::same_as<std::expected<std::string, TextResourceError>>;
};

template <class Resolver>
concept KeyResourceResolver = requires(Resolver &resolver, std::string_view key) {
  { std::invoke(resolver, key) } -> std::same_as<std::expected<std::string, TextResourceError>>;
};

template <class... Values> struct LocalizedValuePattern {
  TextValue expression;
  std::function<std::expected<std::string, TextResourceError>(std::string_view, const Values &...)> resolver;
  bool hasResolver{true};
};

template <class... Values> struct ValueTextRuntime {
  std::string pattern = std::string("", 0);
  std::shared_ptr<const LocalizedValuePattern<Values...>> localization;
  std::optional<TextResourceError> resourceFailure;
  std::tuple<ValueTextInput<Values>...> inputs;
  std::array<std::uint64_t, sizeof...(Values)> revisions{};
  bool initialized{};
  WidthPolicy policy;
  ManagedState<std::string> output;

  std::expected<std::shared_ptr<const std::string>, TreeError> render(bool onlyChanged) {
    auto snapshots = std::apply(
      [](const auto &...input) {
        return std::tuple(input.read()...);
      },
      inputs);
    const auto failure = std::apply(
      [](const auto &...snapshot) -> std::optional<TreeError> {
        std::optional<TreeError> error;

        ((snapshot ? void() : void(error = snapshot.error())), ...);

        return error;
      },
      snapshots);
    if (failure) {
      return std::unexpected(*failure);
    }

    const auto current = std::apply(
      [](const auto &...snapshot) {
        return std::array<std::uint64_t, sizeof...(Values)>{(*snapshot)->revision...};
      },
      snapshots);
    if (onlyChanged && initialized && current == revisions) {
      return std::shared_ptr<const std::string>{};
    }

    std::optional<GraphemeText> localized;
    std::string_view selectedPattern = pattern;

    resourceFailure.reset();
    if (localization) {
      auto resolved =
        localization->hasResolver
          ? localization->expression.resolve(policy,
                                             [&](std::string_view key) {
                                               return std::apply(
                                                 [&](const auto &...snapshot) {
                                                   return localization->resolver(key, (*snapshot)->value...);
                                                 },
                                                 snapshots);
                                             })
          : localization->expression.resolve(policy);
      if (!resolved) {
        if (const auto *resource = std::get_if<TextResourceError>(&resolved.error())) {
          resourceFailure = *resource;
        }

        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      localized.emplace(std::move(*resolved));
      selectedPattern = localized->bytes();
    }

    const auto bytes = std::apply(
      [&](const auto &...snapshot) {
        return std::vformat(selectedPattern, std::make_format_args((*snapshot)->value...));
      },
      snapshots);
    auto validated = GraphemeText::from(bytes, policy);
    if (!validated) {
      const auto kind = validated.error().kind;
      const bool limited = kind == TextErrorKind::INPUT_LIMIT || kind == TextErrorKind::CLUSTER_LIMIT ||
                           kind == TextErrorKind::GRAPHEME_LIMIT || kind == TextErrorKind::CELL_LIMIT;

      return std::unexpected(limited ? TreeError::NODE_LIMIT : TreeError::INVALID_ARGUMENT);
    }

    auto result = std::make_shared<const std::string>(bytes);

    revisions = current;
    initialized = true;

    return result;
  }

  std::expected<void, TreeError> update() {
    const auto previous = revisions;
    auto candidate = render(true);
    if (!candidate) {
      return std::unexpected(candidate.error());
    }
    if (previous == revisions) {
      return {};
    }

    auto changed = output.set(**candidate);
    if (!changed) {
      revisions = previous;

      return std::unexpected(changed.error());
    }

    // A delivery rejection is postcommit. Existing downstream recovery reads the committed snapshot.
    return {};
  }
};

struct ValueTextAccess {
  template <class T>
  static std::expected<ValueTextInput<T>, ManagedError> input(ManagedScene &scene, Node<Widget> node,
                                                              const Model<T> &source, MountContext &context) {
    auto state = ValueModelAccess::resolve(scene, source, context);
    if (!state) {
      return std::unexpected(state.error());
    }

    ValueTextInput<T> result;

    result.root = ValueModelAccess::identity(source);
    result.read = [state = *state] {
      return state.snapshot();
    };
    result.attach = [&scene, node, state = *state](auto callback, const auto &graph) {
      return Access::observeValueText(scene, node, state, std::move(callback), graph);
    };

    return result;
  }

  template <class T>
  static auto input(ManagedScene &scene, Node<Widget> node, const ModelValue<T> &source, MountContext &context) {
    return input(scene, node, ValueModelAccess::root(source), context);
  }

  template <class T, class Member>
  static std::expected<ValueTextInput<std::remove_cv_t<Member>>, ManagedError>
  input(ManagedScene &scene, Node<Widget> node, const ModelField<T, Member> &source, MountContext &context) {
    using Value = std::remove_cv_t<Member>;
    const auto member = ValueModelAccess::member(source);
    if (!member) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }

    auto root = input(scene, node, ValueModelAccess::root(source), context);
    if (!root) {
      return std::unexpected(root.error());
    }

    ValueTextInput<Value> result;

    result.root = root->root;
    result.attach = std::move(root->attach);
    result.read = [read = std::move(root->read), member]() -> typename ValueTextInput<Value>::Read {
      auto snapshot = read();
      if (!snapshot) {
        return std::unexpected(snapshot.error());
      }

      return std::make_shared<const PropertyChange<Value>>((*snapshot)->value.*member, (*snapshot)->revision);
    };

    return result;
  }

  template <class Value>
    requires(!portals::detail::IS_LIVE_TEXT_SOURCE<Value>)
  static std::expected<ValueTextInput<Value>, ManagedError> input(ManagedScene &, Node<Widget>, const Value &value,
                                                                  MountContext &) {
    auto snapshot = std::make_shared<const PropertyChange<Value>>(value, 0);
    ValueTextInput<Value> result;

    result.read = [snapshot]() -> typename ValueTextInput<Value>::Read {
      return snapshot;
    };

    return result;
  }

  template <class... Args>
  static std::expected<ManagedState<std::string>, ManagedError>
  create(ManagedScene &scene, Node<Widget> node, MountContext &context,
         const std::shared_ptr<Access::ValueCaptionGraph> &graph, const std::string &pattern,
         const std::tuple<Args...> &arguments,
         std::shared_ptr<const LocalizedValuePattern<TextArgumentValue<Args>...>> localization = {}) {
    using Runtime = ValueTextRuntime<TextArgumentValue<Args>...>;
    const auto owner = Access::lifetime(scene);
    auto runtime = std::make_shared<Runtime>();

    runtime->pattern = pattern;
    runtime->localization = std::move(localization);
    runtime->policy = scene.widthPolicy();

    std::optional<ManagedError> failure;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      const auto resolve = [&]<std::size_t Index>() {
        if (failure) {
          return;
        }

        auto resolved = input(scene, node, std::get<Index>(arguments), context);
        if (owner.expired()) {
          failure = ManagedError{ManagedPhase::BIND, node.id(), TreeError::CLOSED, {}, {}};
        } else if (!resolved) {
          failure = resolved.error();
        } else {
          std::get<Index>(runtime->inputs) = std::move(*resolved);
        }
      };

      (resolve.template operator()<I>(), ...);
    }(std::index_sequence_for<Args...>{});
    if (failure) {
      return std::unexpected(*failure);
    }

    std::vector<const void *> roots(std::initializer_list<const void *>{});

    roots.reserve(sizeof...(Args));
    std::apply(
      [&](const auto &...input) {
        const auto add = [&](const auto &value) {
          if (value.root && std::ranges::find(roots, value.root.get()) == roots.end()) {
            roots.push_back(value.root.get());
          }
        };

        (add(input), ...);
      },
      runtime->inputs);
    auto admitted = Access::admitValueTextObservers(scene, node, roots.size());
    if (!admitted) {
      return std::unexpected(admitted.error());
    }

    graph->observers.reserve(roots.size());
    auto initial = Access::computeValueText(scene, [&] {
      return runtime->render(false);
    });
    if (!initial) {
      auto initialFailure = ManagedError{ManagedPhase::BIND, node.id(), initial.error(), {}, {}};

      initialFailure.resource = runtime->resourceFailure;

      return std::unexpected(initialFailure);
    }

    auto output = Access::valueTextState(scene, **initial, graph);
    if (!output) {
      return std::unexpected(output.error());
    }

    runtime->output = *output;
    graph->payload = runtime;
    roots.clear();
    std::apply(
      [&](auto &...input) {
        const auto attach = [&](auto &value) {
          if (failure || !value.root || std::ranges::find(roots, value.root.get()) != roots.end()) {
            return;
          }

          auto observed = value.attach(
            [weak = std::weak_ptr(runtime)]() -> std::expected<void, TreeError> {
              const auto entered = weak.lock();

              return entered ? entered->update() : std::expected<void, TreeError>{};
            },
            graph);
          if (!observed) {
            failure = observed.error();
          } else {
            roots.push_back(value.root.get());
          }
        };

        (attach(input), ...);
      },
      runtime->inputs);

    return failure ? std::expected<ManagedState<std::string>, ManagedError>(std::unexpected(*failure)) : output;
  }

  template <class... Args> static ValueText make(std::string_view pattern, Args &&...args) {
    auto arguments =
      std::make_shared<const std::tuple<OwnedTextArgument<Args>...>>(retainedTextArgument(std::forward<Args>(args))...);
    auto ownedPattern = std::make_shared<const std::string>(pattern);
    ValueText result;

    result.data_ = std::make_shared<ValueTextDeclaration>();
    result.data_->create = [arguments, ownedPattern](ManagedScene &scene, Node<Widget> node, MountContext &context,
                                                     const auto &graph) {
      return create(scene, node, context, graph, *ownedPattern, *arguments);
    };

    return result;
  }

  template <class Resolver, class... Args> static ValueText make(TextValue pattern, Resolver resolver, Args &&...args) {
    auto arguments =
      std::make_shared<const std::tuple<OwnedTextArgument<Args>...>>(retainedTextArgument(std::forward<Args>(args))...);
    bool hasResolver = true;
    if constexpr (std::is_pointer_v<Resolver>) {
      hasResolver = resolver != nullptr;
    } else if constexpr (requires { resolver.operator bool(); }) {
      hasResolver = resolver.operator bool();
    }
    auto adapted = [resolver = std::move(resolver)](std::string_view key,
                                                    [[maybe_unused]] const TextArgumentValue<Args> &...values) mutable {
      if constexpr (ValueResourceResolver<Resolver, TextArgumentValue<Args>...>) {
        return std::invoke(resolver, key, values...);
      } else {
        return std::invoke(resolver, key);
      }
    };
    auto localization = std::make_shared<const LocalizedValuePattern<TextArgumentValue<Args>...>>(
      std::move(pattern), std::move(adapted), hasResolver);
    ValueText result;

    result.data_ = std::make_shared<ValueTextDeclaration>();
    result.data_->create = [arguments, localization](ManagedScene &scene, Node<Widget> node, MountContext &context,
                                                     const auto &graph) {
      return create(scene, node, context, graph, std::string("", 0), *arguments, localization);
    };

    return result;
  }

  template <class Receiver>
  static std::expected<void, ManagedError> bind(ManagedScene &scene, Node<Receiver> node, const ValueText &source,
                                                MountContext &context, const void *previous = nullptr) {
    if (!source.data_) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }

    const auto owner = Access::lifetime(scene);
    const auto declaration = source.data_;
    auto graph = std::make_shared<Access::ValueCaptionGraph>();
    auto metadata = std::make_shared<ValueCaptionBinding>();

    graph->scene = &scene;
    graph->lifetime = owner;
    metadata->type = &VALUE_CAPTION_TYPE<ValueText>;
    metadata->source = declaration;
    metadata->graph = graph;

    auto text = declaration->create(scene, node, context, graph);
    if (!text) {
      return std::unexpected(text.error());
    }
    if (owner.expired()) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::CLOSED, {}, {}});
    }

    return Access::installValueCaption(scene, node, *text, std::move(metadata), previous, context);
  }

  template <class Receiver>
  static std::expected<void, ManagedError> replace(ManagedScene &scene, Node<Receiver> node, const ValueText &source) {
    auto slot = Access::valueCaptionSlot(scene, node);
    if (!slot) {
      return std::unexpected(slot.error());
    }
    if (!source.data_) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }
    if (slot->source && slot->source->type == &VALUE_CAPTION_TYPE<ValueText> && slot->source->source == source.data_) {
      return {};
    }

    MountContext context;
    auto bound = bind(scene, node, source, context, slot->identity);
    if (!bound) {
      return bound;
    }

    ModelAccess::publish(context);

    return {};
  }
};

/// @endcond
} // namespace detail

template <class T>
std::expected<void, ManagedError> Ref<T>::setModel(const ValueText &source, std::source_location origin) const
  requires(std::same_as<T, Label> || std::same_as<T, Button> || std::same_as<T, Border>)
{
  detail::SetupFailureScope mask({}, nullptr, nullptr);
  const auto observation = target();
  auto owner = detail::eventScene(observation);
  if (!owner) {
    return std::unexpected(owner.error());
  }

  const auto node = this->node();
  try {
    auto result = detail::ValueTextAccess::replace(**owner, node, source);
    if (!result) {
      result.error().registrationOrigin = origin;
    }

    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::NO_MEMORY, {}, {}});
  } catch (...) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::CALLBACK_FAILED, {}, {}});
  }
}

template <class T>
bool Ref<T>::model(const ValueText &source, std::source_location origin) const
  requires(std::same_as<T, Label> || std::same_as<T, Button> || std::same_as<T, Border>)
{
  const auto observation = target();
  auto result = setModel(source, origin);
  if (!result) {
    detail::reportCaptionModelError(observation, result.error(), origin,
                                    std::same_as<T, Label>    ? "Label"
                                    : std::same_as<T, Button> ? "Button"
                                                              : "Border");
  }

  return static_cast<bool>(result);
}
} // namespace portals::ui

namespace portals {
/** @brief Declare live formatted text over explicit scalar roots, fields or value properties.
 * @tparam Args One or more owning arguments, including at least one scalar dependency. Static strings
 * and string views are copied; borrowed pointers and unsupported views are rejected.
 * @param pattern Standard format string checked against the underlying argument value types.
 * @param args Owning descriptors and static values retained by this cold expression.
 * @return Owning computed text root, validated under the receiving scene's width policy at mount.
 * Construction may allocate/throw; runtime formatter errors preserve the previous valid caption and
 * follow the existing observer error policy. There is no ambient locale or localization resolver.
 */
template <class... Args>
  requires(sizeof...(Args) > 0) && (detail::IS_LIVE_TEXT_SOURCE<std::remove_cvref_t<Args>> || ...) &&
          (ui::detail::TextFormatArgument<Args> && ...)
[[nodiscard]] ui::ValueText text(std::format_string<const ui::detail::TextArgumentValue<Args> &...> pattern,
                                 Args &&...args) {
  return ui::detail::ValueTextAccess::make(pattern.get(), std::forward<Args>(args)...);
}

/** @brief Resolve an owning localization key and format over explicit scalar dependencies.
 * @tparam Args Owning static arguments and scalar roots, including at least one live dependency.
 * @param pattern Literal format pattern or resource key; translated format strings are checked at runtime.
 * @tparam Resolver Copyable resource adapter accepting key alone or key followed by underlying argument values.
 * @param resolver Synchronous adapter retained by value; no ambient locale or unpublished model is read.
 * @param args Format arguments and explicit dependencies. A changed argument unused by the pattern still
 * triggers resolution, so a locale model can be listed explicitly without appearing in the output.
 * @return Owning computed source. Initial resource failures retain TextResourceError; later observer
 * failures preserve the last valid caption and use the existing TreeError observer channel.
 * Resource access and formatters must be short, memory-only and non-reentrant on the UI thread.
 */
template <class Resolver, class... Args>
  requires(sizeof...(Args) > 0) && (detail::IS_LIVE_TEXT_SOURCE<std::remove_cvref_t<Args>> || ...) &&
          (ui::detail::TextFormatArgument<Args> && ...) && std::copy_constructible<Resolver> &&
          (ui::detail::KeyResourceResolver<Resolver> ||
           ui::detail::ValueResourceResolver<Resolver, ui::detail::TextArgumentValue<Args>...>)
[[nodiscard]] ui::ValueText text(TextValue pattern, Resolver resolver, Args &&...args) {
  return ui::detail::ValueTextAccess::make(std::move(pattern), std::move(resolver), std::forward<Args>(args)...);
}
} // namespace portals

namespace portals::ui {
// The declaration also participates in ADL for Model/ModelField arguments without introducing a second overload.
using portals::text;

// Public contracts are documented on the Description declarations; these definitions require complete value types.
/// @cond INTERNAL

template <class T>
template <class Title>
  requires std::same_as<Title, ValueText>
Description<T> &&Description<T>::framed(std::string_view pattern, Title title) &&
  requires(std::derived_from<T, SizedWidget> && !std::derived_from<T, ScrollArea>)
{
  (void)std::move(*this).framed(pattern);
  if (state_) {
    frameRecipe().titleBinding = [title = std::move(title)](ManagedScene &scene, Node<Border> node,
                                                            detail::MountContext &context) {
      return detail::ValueTextAccess::bind(scene, node, title, context);
    };
  }

  return std::move(*this);
}

template <class T>
template <class Value>
  requires std::formattable<Value, char> && std::equality_comparable<Value> &&
           std::copy_constructible<Value>
           Description<T> &&Description<T>::framed(std::string_view pattern, Model<Value> title) &&
             requires(std::derived_from<T, SizedWidget> && !std::derived_from<T, ScrollArea>)
{
  (void)std::move(*this).framed(pattern);
  if (state_) {
    frameRecipe().titleBinding = [title = std::move(title)](ManagedScene &scene, Node<Border> node,
                                                            detail::MountContext &context) {
      return detail::ValueModelAccess::bindCaption(scene, node, title, std::make_shared<std::identity>(), context);
    };
  }

  return std::move(*this);
}

template <class T>
template <class Title>
  requires std::same_as<Title, ValueText>
Description<T> &&Description<T>::framed(Ref<Border> outer, std::string_view pattern, Title title) &&
  requires(std::derived_from<T, SizedWidget> && !std::derived_from<T, ScrollArea>)
{
  (void)std::move(*this).framed(pattern, std::move(title));
  if (state_) {
    frameRecipe().reference = std::move(outer);
  }

  return std::move(*this);
}

template <class T>
template <class Value>
  requires std::formattable<Value, char> && std::equality_comparable<Value> &&
           std::copy_constructible<Value>
           Description<T> &&Description<T>::framed(Ref<Border> outer, std::string_view pattern, Model<Value> title) &&
             requires(std::derived_from<T, SizedWidget> && !std::derived_from<T, ScrollArea>)
{
  (void)std::move(*this).framed(pattern, std::move(title));
  if (state_) {
    frameRecipe().reference = std::move(outer);
  }

  return std::move(*this);
}

/// @endcond

} // namespace portals::ui
