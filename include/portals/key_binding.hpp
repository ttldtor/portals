// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <initializer_list>
#include <portals/managed_diagnostics.hpp>
#include <portals/shortcut.hpp>
#include <portals/text_value.hpp>
#include <portals/widget.hpp>
#include <thread>

namespace portals {
class ManagedScene;
class KeyBinding;

namespace ui {
class KeyInfo;
class KeyBindingRef;
} // namespace ui

namespace detail {
struct KeyBindingSlot;
}

/** @brief Scene-local, never reused registration identity; not a persistence key. */
class KeyBindingId {
public:
  /** @brief Construct an invalid identity. */
  KeyBindingId() = default;
  /** @param other Identity to compare. @return Exact scene and registration equality. */
  bool operator==(const KeyBindingId &other) const = default;

private:
  friend class ManagedScene;
  NodeId scene_{};
  std::uint64_t serial_{};
};

/** @brief Explicit terminal phase knowledge; UNKNOWN does not suppress routing. */
enum class KeySupport {
  UNKNOWN,    ///< No explicit support claim; annotated routing remains enabled.
  SUPPORTED,  ///< The application declares this native phase supported.
  UNSUPPORTED ///< The application explicitly excludes this annotated phase.
};

/** @brief Application-supplied native phase support, independent of physical-key identity. */
struct KeyCapabilities {
  KeySupport press{KeySupport::UNKNOWN};   ///< Native PRESS support.
  KeySupport repeat{KeySupport::UNKNOWN};  ///< Native REPEAT support.
  KeySupport release{KeySupport::UNKNOWN}; ///< Native RELEASE support.
  /** @param other Capabilities to compare. @return Equality of all three phase declarations. */
  bool operator==(const KeyCapabilities &other) const = default;
};

/** @brief Same exact resolver type as ui::TextResolver; used only during checked normalization. */
using KeyTextResolver = std::function<std::expected<std::string, TextResourceError>(std::string_view)>;

/** @brief Owning metadata normalized once before canonical publication. */
struct KeyInfo {
  /** @param value Required nonempty label expression. */
  explicit KeyInfo(TextValue value) : label(std::move(value)) {
  }

  TextValue label;                           ///< Required resolved single-line label.
  TextValue description{std::string_view{}}; ///< Optional owning explanation.
  KeyTextResolver resolver{};                ///< Not retained after successful normalization.
  Node<Widget> owner{};                      ///< Empty selects the scene root.
  Node<Widget> scope{};                      ///< Empty is global; otherwise an exact declared focus scope.
  bool enabled{true};                        ///< Disabled commands remain visible in full help.
  bool visible{true};                        ///< Hidden commands neither route nor appear in projections.
private:
  friend class ManagedScene;
  friend class ui::KeyInfo;
  std::shared_ptr<detail::KeyBindingSlot> publication_;
  std::weak_ptr<const void> scopeIdentity_;
};

/** @brief Eligibility observed for one immutable snapshot, not a universal conflict winner. */
enum class KeyEligibility {
  ELIGIBLE,            ///< Eligible for matching; conflict winner still depends on the event.
  DISABLED,            ///< Explicitly disabled, retained in full help.
  HIDDEN,              ///< Explicitly hidden from routing and projections.
  UNBOUND,             ///< Identity retained without any key alternatives.
  OWNER_INELIGIBLE,    ///< Live owner or ancestor is hidden or disabled.
  OWNER_RETIRED,       ///< Original generational owner no longer exists.
  SCOPE_INACTIVE,      ///< Original scope is disconnected, ineligible or outside current focus.
  TERMINAL_UNSUPPORTED ///< Every annotated phase is explicitly unsupported.
};
/** @brief Conditional alternative overlap. */
enum class KeyConflictKind {
  EXACT,           ///< Both alternatives end at their shared strokes.
  TERMINAL_PREFIX, ///< One alternative ends while the other continues.
  SHARED_PREFIX    ///< Alternatives compete initially and diverge later.
};

/** @brief One bounded pair relation; ordinals refer only to its owning snapshot. */
struct KeyConflict {
  /** @brief Release this independent owning relation. */
  ~KeyConflict() = default;
  std::size_t alternative{};             ///< This alternative's index.
  std::size_t otherOrdinal{};            ///< Other row's snapshot ordinal.
  std::optional<KeyBindingId> otherId{}; ///< Absent for legacy registrations.
  std::size_t otherAlternative{};        ///< Other alternative's index.
  KeyConflictKind kind{};                ///< Exact, terminal prefix or shared-prefix competition.
  std::size_t sharedStrokes{};           ///< Number of common leading strokes.
  bool thisIsNewer{};                    ///< Registration priority, independent of scope depth.
  bool thisTerminalAtPrefix{};           ///< This alternative ends at the common prefix.
  bool otherTerminalAtPrefix{};          ///< Other alternative ends at the common prefix.
  std::vector<KeyAction> overlapPhases = std::vector<KeyAction>(0); ///< Actual overlapping native phases.
};

/** @brief Owning read DTO, retaining no callback, resolver, subscription or widget. */
struct KeyBindingView {
  /** @param caption Resolved label. @param detail Resolved description. */
  KeyBindingView(GraphemeText caption, GraphemeText detail)
      : label(std::move(caption)), description(std::move(detail)) {
  }

  /** @brief Release independently owned text, alternatives and relations. */
  ~KeyBindingView() = default;
  std::optional<KeyBindingId> id{};                           ///< Absent for legacy registrations.
  std::size_t ordinal{};                                      ///< Display-local registration position.
  GraphemeText label;                                         ///< Empty for unannotated legacy bindings.
  GraphemeText description;                                   ///< Independently owned text.
  std::optional<KeyEvent> legacy{};                           ///< Original exact legacy key, if any.
  std::optional<Shortcut> shortcut{};                         ///< Owning normalized alternatives, including sequences.
  std::vector<KeyAction> actions = std::vector<KeyAction>(0); ///< Native admitted phases.
  NodeId owner{};                                             ///< Diagnostic identity only.
  NodeId scope{};                                             ///< Diagnostic identity only.
  KeyEligibility eligibility{KeyEligibility::ELIGIBLE};       ///< Observed admission reason.
  std::vector<KeyConflict> conflicts = std::vector<KeyConflict>(0); ///< Conditional alternative relations.
};

/** @brief Independent historical read snapshot; scope disconnect is observed on the next UI admission/query. */
struct KeyBindingsSnapshot {
  /** @brief Release this historical owning projection. */
  ~KeyBindingsSnapshot() = default;
  std::uint64_t revision{};        ///< Canonical metadata revision.
  std::uint64_t focusGeneration{}; ///< Recovered focus observation.
  std::uint64_t inputGeneration{}; ///< Tree eligibility observation.
  std::uint64_t scopeGeneration{}; ///< Exact cancellation-identity observation revision.
  KeyCapabilities capabilities{};  ///< Explicit phase knowledge.
  std::vector<KeyBindingView> bindings =
    std::vector<KeyBindingView>(std::initializer_list<KeyBindingView>{}); ///< Owning rows in registration order.
};

/** @brief Weak UI-affine command observation; destruction never unregisters the command. */
class KeyBinding {
public:
  /** @brief Construct an empty, allocation-free observation. */
  KeyBinding() = default;

  /** @return Immutable identity without reading the scene. */
  KeyBindingId id() const noexcept {
    return id_;
  }

  /** @param value New admission state. @return Pure checked result; equal state is a no-op. */
  std::expected<void, ManagedError> setEnabled(bool value) const;
  /** @param value Visibility and admission state. @return Pure checked result. */
  std::expected<void, ManagedError> setVisible(bool value) const;
  /** @param label Required label. @param description Optional explanation. @param resolver Local resource resolver.
   * @return Atomic normalized text publication; active prefix is preserved. */
  std::expected<void, ManagedError> setText(TextValue label, TextValue description,
                                            KeyTextResolver resolver = {}) const;
  /** @param selection Owning checked alternatives/phases. @return Atomic rebind or exact typed failure. */
  std::expected<void, ManagedError> rebind(const KeySelection &selection) const;
  /** @return Success after clearing alternatives, keeping identity and explanation. */
  std::expected<void, ManagedError> unbind() const;
  /** @return Success once; subsequent mutations return STALE_NODE. */
  std::expected<void, ManagedError> remove() const;

private:
  friend class ManagedScene;
  friend class ui::KeyBindingRef;
  std::expected<ManagedScene *, ManagedError> scene() const;
  ManagedScene *scene_{};
  std::weak_ptr<const void> lifetime_;
  std::thread::id owner_{};
  KeyBindingId id_;
};

namespace detail {
/// @cond INTERNAL
struct KeyBindingSlot {
  const std::thread::id owner = std::this_thread::get_id();
  std::optional<KeyBinding> binding;
};

/// @endcond
} // namespace detail

namespace ui {
/** @brief Deferred observing slot, published only by a complete successful registration.
 * Construction may allocate; copies never own a scene or action. */
class KeyBindingRef {
public:
  /** @brief Create an unpublished slot; may throw std::bad_alloc. */
  KeyBindingRef() : slot_(std::make_shared<portals::detail::KeyBindingSlot>()) {
  }

  /** @return Published immutable identity, CLOSED or WRONG_THREAD. */
  std::expected<KeyBindingId, ManagedError> id() const;
  /** @param value New state. @param origin Call site. @return Pure checked publication. */
  std::expected<void, ManagedError> setEnabled(bool value,
                                               std::source_location origin = std::source_location::current()) const;
  /** @param value New state. @param origin Call site. @return Pure checked publication. */
  std::expected<void, ManagedError> setVisible(bool value,
                                               std::source_location origin = std::source_location::current()) const;
  /** @param label New label. @param description New explanation. @param resolver Local resolver.
   * @param origin Call site. @return Pure checked publication. */
  std::expected<void, ManagedError> setText(TextValue label, TextValue description, KeyTextResolver resolver = {},
                                            std::source_location origin = std::source_location::current()) const;
  /** @param selection New alternatives. @param origin Call site. @return Pure checked publication. */
  std::expected<void, ManagedError> rebind(const KeySelection &selection,
                                           std::source_location origin = std::source_location::current()) const;
  /** @param origin Call site. @return Pure checked unbinding. */
  std::expected<void, ManagedError> unbind(std::source_location origin = std::source_location::current()) const;
  /** @param origin Call site. @return Pure checked retirement. */
  std::expected<void, ManagedError> remove(std::source_location origin = std::source_location::current()) const;

private:
  friend class KeyInfo;
  std::expected<KeyBinding, ManagedError> binding() const;
  std::shared_ptr<portals::detail::KeyBindingSlot> slot_;
};
} // namespace ui
} // namespace portals
