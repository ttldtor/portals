// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_description.hpp>

namespace portals::ui {

/** @brief Owning class list stage using the same ordered managed setup as Description::classes. */
struct ClassesModifier {
  /** @brief Destroy owning names; checked container transfers remain fallible. */
  ~ClassesModifier() = default;

  detail::ClassNames values; ///< Owning ordered names, including duplicates; checked transfers remain fallible.

  /** @tparam T Public Widget subtype, including final custom widgets.
   * @param description Owning recipe consumed by this stage.
   * @param modifier Names copied when the stage was constructed; empty clears classes.
   * @return Consuming recipe preserving other metadata and source-order style replacement.
   */
  template <class T> friend Description<T> operator|(Description<T> &&description, ClassesModifier modifier) {
    return std::move(description).setup([values = modifier.values](ManagedScene &scene, Node<T> node) mutable {
      return scene.classes(node, values.values);
    });
  }
};

/** @param value Class names copied and split now on ASCII whitespace; empty clears classes.
 * @return Owning consuming stage equivalent to Description::classes, preserving order and duplicates.
 */
inline ClassesModifier classes(std::string_view value) {
  return {detail::splitClasses(value)};
}

/** @brief Owning local instance-state stage for every widget recipe. */
struct InstanceStateModifier {
  bool visibility{}; ///< Select visibility when true, enabled state otherwise.
  bool value{true};  ///< Final local flag value; no runtime borrow.

  /** @tparam T Widget subtype, including final widgets without a size policy.
   * @param description Owning recipe consumed by this stage.
   * @param modifier Local state update; the last modifier for each flag wins.
   * @return Updated recipe sharing the existing mount transaction.
   */
  template <class T> friend Description<T> operator|(Description<T> &&description, InstanceStateModifier modifier) {
    if (modifier.visibility) {
      return std::move(description).visible(modifier.value);
    }

    return std::move(description).enabled(modifier.value);
  }
};

/** @param value Local visibility, defaulting to true.
 * @return Consuming stage equivalent to Description::visible.
 */
constexpr InstanceStateModifier visible(bool value = true) {
  return {true, value};
}

/** @param value Local enabled state, defaulting to true.
 * @return Consuming stage equivalent to Description::enabled.
 */
constexpr InstanceStateModifier enabled(bool value = true) {
  return {false, value};
}

/** @brief Owning compact size stage; uses the same methods and existing solver as ordinary composition. */
struct SizeModifier {
  detail::SizeChange change; ///< Owning checked policy operation; no scene/widget borrow.

  /** @tparam T SizedWidget subtype described by the consuming input.
   * @param description Owning recipe consumed by this stage.
   * @param modifier Owning axis/weight update, applied in pipe order at mount.
   * @return Updated owning recipe; invalid values preserve transactional mount failure semantics.
   */
  template <std::derived_from<SizedWidget> T>
  friend Description<T> operator|(Description<T> &&description, SizeModifier modifier) {
    switch (modifier.change.dimension) {
    case detail::SizeDimension::WIDTH:
      return std::move(description).width(modifier.change.extent);
    case detail::SizeDimension::HEIGHT:
      return std::move(description).height(modifier.change.extent);
    case detail::SizeDimension::FLEX_X:
      return std::move(description).flexX(modifier.change.weight);
    case detail::SizeDimension::FLEX_Y:
      return std::move(description).flexY(modifier.change.weight);
    case detail::SizeDimension::FLEX:
      return std::move(description).flex(modifier.change.weight);
    }

    return std::move(description).flex(0);
  }
};

/** @param cells Nonnegative fixed horizontal extent; checked at mount.
 * @return Owning size stage replacing only the horizontal policy.
 */
constexpr SizeModifier width(std::int32_t cells) {
  return {{detail::SizeDimension::WIDTH, cells}};
}

/** @param cells Nonnegative fixed vertical extent; checked at mount.
 * @return Owning size stage replacing only the vertical policy.
 */
constexpr SizeModifier height(std::int32_t cells) {
  return {{detail::SizeDimension::HEIGHT, cells}};
}

/** @param weight Positive horizontal flex weight; checked at mount.
 * @return Owning zero-basis flex stage preserving existing bounds and vertical policy.
 */
constexpr SizeModifier flexX(std::uint32_t weight = 1) {
  return {{detail::SizeDimension::FLEX_X, 0, weight}};
}

/** @param weight Positive vertical flex weight; checked at mount.
 * @return Owning zero-basis flex stage preserving existing bounds and horizontal policy.
 */
constexpr SizeModifier flexY(std::uint32_t weight = 1) {
  return {{detail::SizeDimension::FLEX_Y, 0, weight}};
}

/** @param weight Positive weight for both axes; checked at mount.
 * @return Owning zero-basis flex stage preserving existing bounds.
 */
constexpr SizeModifier flex(std::uint32_t weight = 1) {
  return {{detail::SizeDimension::FLEX, 0, weight}};
}

} // namespace portals::ui
