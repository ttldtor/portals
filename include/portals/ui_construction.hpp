// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_description.hpp>

namespace portals::ui {

/** @brief Capture constructor arguments without requiring inheritance from a DSL widget.
 * @tparam T Public Widget subtype, including final types, constructible from the owned arguments.
 * @tparam Args Decayed owning argument types; string-like inputs are materialized as std::string.
 * @param args Values copied/moved now and moved into T at mount. Other pointers/reference_wrapper
 * remain explicit borrows: their referents must outlive the widget. No deep copy is inferred.
 * @return A single-use recipe. T must own any string_view it retains beyond its constructor.
 */
template <class T, class... Args> Description<T> custom(Args &&...args) {
  using OwnedArguments = std::tuple<decltype(detail::own(std::forward<Args>(args)))...>;
  constexpr bool HAS_TEXT_ARGUMENT = (std::convertible_to<Args, std::string_view> || ...);

  // Construct strings directly in stationary tuple storage. Moving a string or tuple<string>
  // can allocate a checked MSVC proxy despite noexcept; a string_view constructor stays fallible.
  // Explicit element types also preserve tuple/pair constructor arguments without CTAD flattening.
  auto owned = [&] {
    if constexpr (HAS_TEXT_ARGUMENT && detail::CHECKED_STRING_MOVES) {
      return std::make_unique<OwnedArguments>(detail::constructionArgument(std::forward<Args>(args))...);
    } else {
      return OwnedArguments(detail::own(std::forward<Args>(args))...);
    }
  }();

  return Description<T>([owned = std::move(owned)](ManagedScene &scene, Node<Widget> parent) mutable {
    auto &arguments = [&]() -> auto & {
      if constexpr (HAS_TEXT_ARGUMENT && detail::CHECKED_STRING_MOVES) {
        return *owned;
      } else {
        return owned;
      }
    }();

    return std::apply(
      [&](auto &...values) {
        return scene.emplace<T>(parent, std::move(values)...);
      },
      arguments);
  });
}

/** @tparam T Public widget subtype compatible with the forwarded constructor arguments.
 * @tparam Args Owning constructor argument types, following the existing custom factory contract.
 * @param reference Empty observing slot published after complete mount.
 * @param args Constructor arguments forwarded into owning recipe storage.
 * @return Existing custom recipe with deferred reference publication.
 */
template <class T, class... Args> Description<T> custom(Ref<T> reference, Args &&...args) {
  return custom<T>(std::forward<Args>(args)...).reference(std::move(reference));
}

} // namespace portals::ui
