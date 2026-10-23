// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_ref.hpp>
#include <portals/key_binding.hpp>

namespace portals::ui {
/** @brief Consuming metadata for one canonical annotated registration; refs resolve after complete mount. */
class KeyInfo {
public:
  /** @param label Owning required label. @param resolver Local resource resolver, never retained by a row. */
  explicit KeyInfo(TextValue label, TextResolver resolver = {}) : value_(std::move(label)) {
    value_.resolver = std::move(resolver);
  }

  /** @param text Owning explanation. @return This consuming metadata. */
  KeyInfo &&description(TextValue text) && {
    value_.description = std::move(text);
    return std::move(*this);
  }

  /** @param reference Deferred command slot. @return This consuming metadata. */
  KeyInfo &&reference(KeyBindingRef reference) && {
    value_.publication_ = std::move(reference.slot_);
    return std::move(*this);
  }

  /** @tparam T Widget type. @param reference Original owner observation. @return This consuming metadata. */
  template <class T>
    requires std::derived_from<T, Widget>
  KeyInfo &&owner(const Ref<T> &reference) && {
    owner_ = reference.target();
    return std::move(*this);
  }

  /** @tparam T Widget type. @param reference Declared scope observation. @return This consuming metadata. */
  template <class T>
    requires std::derived_from<T, Widget>
  KeyInfo &&scope(const Ref<T> &reference) && {
    scope_ = reference.target();
    return std::move(*this);
  }

  /** @param value New admission state. @return This consuming metadata. */
  KeyInfo &&enabled(bool value) && {
    value_.enabled = value;
    return std::move(*this);
  }

  /** @param value New visibility/admission state. @return This consuming metadata. */
  KeyInfo &&visible(bool value) && {
    value_.visible = value;
    return std::move(*this);
  }

private:
  friend class Scene;

  std::expected<portals::KeyInfo, ManagedError> resolve(ManagedScene &scene) {
    for (const auto &target : {owner_, scope_}) {
      if (!target) {
        continue;
      }
      auto owner = detail::eventScene(target);
      if (!owner) {
        return std::unexpected(owner.error());
      }
      if (*owner != &scene) {
        return std::unexpected(
          ManagedError{ManagedPhase::BIND, target->node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
      }
    }
    if (owner_) {
      value_.owner = owner_->node;
    }
    if (scope_) {
      value_.scope = scope_->node;
    }
    return std::move(value_);
  }

  portals::KeyInfo value_;
  std::shared_ptr<detail::RefTarget> owner_, scope_;
};

/** @param label Literal bytes copied now. @return Owning consuming metadata. */
inline KeyInfo keyInfo(std::string_view label) {
  return KeyInfo(TextValue(label));
}

/** @param label Owning expression. @param resolver Local resolver. @return Owning consuming metadata. */
inline KeyInfo keyInfo(TextValue label, TextResolver resolver = {}) {
  return KeyInfo(std::move(label), std::move(resolver));
}


} // namespace portals::ui
