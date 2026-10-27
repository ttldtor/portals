// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/managed.hpp>

namespace portals::ui {
/** @brief Owning shared command presentation; shortcut text does not install a global binding. */
struct ActionMetadata {
  /** @brief Keep fallible metadata copies outside implicit noexcept transfers. */
  ~ActionMetadata() = default;
  std::string caption = std::string(0, '\0');        ///< Shared button caption.
  std::string shortcut = std::string(0, '\0');       ///< Display text; bind input explicitly with onKey.
  std::string help = std::string(0, '\0');           ///< Owning help text.
  bool enabled{true};                                ///< Immediate callback admission gate.
  bool visible{true};                                ///< Shared presenter visibility and invocation admission.
  bool checked{};                                    ///< Shared selected presentation state.
  std::string exclusiveGroup = std::string(0, '\0'); ///< Empty means independent selection.
};

/** @brief Positive finite admission budgets for one owning set. */
struct ActionSetOptions {
  std::size_t maxActions{256};         ///< Live action count.
  std::size_t maxPresenters{1024};     ///< Live button projections and menu entries, including duplicates.
  std::size_t maxMetadataBytes{65536}; ///< Live keys/current metadata; external snapshots are independent.
  std::size_t maxKeyBytes{256};        ///< Per-action key byte limit.
};

/** @brief Owning synchronous invocation context; no widget or scene ownership. */
struct ActionInvocation {
  ClickSource source{ClickSource::PROGRAMMATIC}; ///< Original explicit, keyboard or pointer source.
  InputModifiers modifiers{};                    ///< Original input intent where supplied.
};

class ActionRef;

namespace detail {
/// @cond INTERNAL
struct QuitCommand;
struct ActionStore;
struct ActionRecord;
struct MountContext;
struct MenuRegistration;

/** @brief Internal bridge to existing managed admission, routing and error receipts. */
struct ActionAccess {
  static EditorCommandResult finish(ManagedScene &, std::expected<void, TreeError>);
  static EditorCommandResult finish(ManagedScene &, std::expected<void, ManagedError>);
  static EditorCommandResult finish(ManagedScene &, EditorCommandResult);
  static std::expected<void, ManagedError> attach(ManagedScene &, const ActionRef &);
  static std::expected<void, ManagedError> button(ManagedScene &, Node<Button>, const ActionRef &);
  static std::expected<void, ManagedError> menu(ManagedScene &, Node<Menu>, MountContext &);
  static std::expected<void, ManagedError> stageMenu(ManagedScene &, MenuRegistration &);
  static std::expected<void, ManagedError> validateMenu(ManagedScene &, const MenuRegistration &);
  static void rollbackMenu(ManagedScene &, const MenuRegistration &);
  static EventReply routeMenu(ManagedScene &, Node<Menu>, Menu &, const RoutedEvent &);
  static std::expected<void, TreeError> prepare(ManagedScene &);
  static void retire(ManagedScene &) noexcept;
  static EditorCommandResult invoke(const ActionRef &, ActionInvocation, std::source_location);
  static EditorCommandResult report(const ActionRef &, std::expected<void, TreeError>, std::source_location);
  static std::expected<void, TreeError> live(const ActionStore &);
  static std::expected<void, TreeError> dirty(const ActionStore &);
  static std::expected<void, TreeError> write(const ActionStore &);
  static std::shared_ptr<ActionStore> retain(const ActionRef &);
};

/// @endcond
} // namespace detail

/** @brief One owning callback and its metadata; construct before checked set admission.
 * Captures and metadata construction may throw before add/action/rebind is entered.
 */
class Action {
public:
  using Result = std::variant<std::expected<void, TreeError>, std::expected<void, ManagedError>,
                              EditorCommandResult>;                 ///< Exact callback outcome.
  using Callback = std::function<Result(const ActionInvocation &)>; ///< Owning compact callback adapter.

  /** @param metadata Owning common presentation.
   * @param callback Compact owning callback, optionally accepting const ActionInvocation&.
   */
  template <class CallbackType>
    requires(std::copy_constructible<CallbackType> && portals::detail::ManagedAction<CallbackType, ActionInvocation>)
  Action(ActionMetadata metadata, CallbackType callback)
      : metadata(std::move(metadata)),
        callback([callback = std::move(callback)]([[maybe_unused]] const ActionInvocation &invocation) mutable -> Result {
          const auto invoke = [&]() -> decltype(auto) {
            if constexpr (std::invocable<CallbackType &, const ActionInvocation &>) {
              return callback(invocation);
            } else {
              return callback();
            }
          };

          if constexpr (std::same_as<decltype(invoke()), void>) {
            invoke();

            return std::expected<void, TreeError>{};
          } else if constexpr (std::same_as<decltype(invoke()), EditorCommandResult> ||
                               std::same_as<decltype(invoke()), std::expected<void, ManagedError>>) {
            return invoke();
          } else {
            auto result = invoke();

            return result ? std::expected<void, TreeError>{} : std::unexpected(result.error());
          }
        }) {
  }

  ActionMetadata metadata; ///< Owning metadata admitted with the callback.
  Callback callback;       ///< Single owning callback; admitted invocations pin this object.
};

/** @brief Weak UI-affine action identity; owner retirement never leaves an executable callback. */
class ActionRef {
public:
  /** @brief Empty weak identity; operations return CLOSED. */
  ActionRef() = default;

  /** @return Owning immutable metadata, or WRONG_THREAD/CLOSED/STALE_NODE/admission failure. */
  std::expected<std::shared_ptr<const ActionMetadata>, TreeError> snapshot() const;
  /** @param metadata Candidate common state, atomically committed after all new storage is ready.
   * @param origin Public mutation site retained in existing diagnostics.
   * @return Checked result, also collected once when the attached scene is live.
   */
  EditorCommandResult set(ActionMetadata metadata, std::source_location origin = std::source_location::current()) const;
  /** @param action Replacement callback and metadata; the currently admitted callback stays alive.
   * @param origin Registration site.
   * @return Checked result; failed preparation preserves the original callback and state.
   */
  EditorCommandResult rebind(Action action, std::source_location origin = std::source_location::current()) const;
  /** @param origin Retirement site. @return Success or typed availability error; self-removal is safe. */
  EditorCommandResult remove(std::source_location origin = std::source_location::current()) const;
  /** @param invocation Explicit original input context.
   * @param origin Invocation site retained for an unreported callback error.
   * @return Existing checked command receipt; hidden/disabled actions succeed without callback entry.
   */
  EditorCommandResult invoke(ActionInvocation invocation = {},
                             std::source_location origin = std::source_location::current()) const;
  /** @param event Existing keyboard input. @return Exact command receipt for existing managed adapters. */
  EditorCommandResult operator()(const KeyEvent &event) const;
  /** @param event Existing button activation. @return Exact command receipt with source/modifiers preserved. */
  EditorCommandResult operator()(const ClickEvent &event) const;

private:
  friend class ActionSet;
  friend struct detail::ActionAccess;
  std::expected<std::shared_ptr<detail::ActionRecord>, TreeError> lock() const;
  std::weak_ptr<detail::ActionStore> store_;
  std::weak_ptr<detail::ActionRecord> record_;
  std::thread::id owner_{};
  std::optional<TreeError> failure_;
};

/** @brief Cold owning declarations, transferred to one managed scene on first button/menu/key binding.
 * UI-affine. Cold recipes and key stages retain declarations until managed attachment.
 * Scene close clears callbacks and invalidates refs even when this descriptor survives.
 * Construction may throw; no router, scheduler, resource executable or binding token is created here.
 */
class ActionSet {
public:
  /** @param options Positive finite admission budgets, checked when admitting an action. */
  explicit ActionSet(ActionSetOptions options = {});
  /** @param other UI-affine declaration owner, left empty; foreign-thread moves terminate. */
  ActionSet(ActionSet &&other) noexcept;
  /** @param other UI-affine declaration owner. @return This owner; foreign-thread assignment terminates. */
  ActionSet &operator=(ActionSet &&other) noexcept;
  ActionSet(const ActionSet &) = delete;
  ActionSet &operator=(const ActionSet &) = delete;
  /** @brief Release cold declarations on UI; attached scenes/recipes retain their own ownership. */
  ~ActionSet();

  /** @param key Nonempty unique organizational key, independent of exclusiveGroup.
   * @param action Owning callback and metadata.
   * @return Weak action or typed failure without partial entry publication.
   */
  std::expected<ActionRef, TreeError> add(std::string_view key, Action action);
  /** @param key Unique organizational key. @param action Owning callback and metadata.
   * @return Compact weak ref; admission failure is delivered at its later scene binding.
   */
  ActionRef action(std::string_view key, Action action);

  /** @param key Unique organizational key. @param caption Shared owning caption copied inside admission.
   * @param callback Compact callback. @return Weak ref with sticky typed failure on rejected admission.
   */
  template <class Callback = detail::QuitCommand>
    requires(std::copy_constructible<Callback> && portals::detail::ManagedAction<Callback, ActionInvocation>)
  ActionRef action(std::string_view key, std::string_view caption, Callback callback) {
    try {
      ActionMetadata metadata;

      metadata.caption = caption;

      return action(key, Action{std::move(metadata), std::move(callback)});
    } catch (const std::bad_alloc &) {
      return failed(TreeError::NO_MEMORY);
    } catch (...) {
      return failed(TreeError::CALLBACK_FAILED);
    }
  }

  /** @param key Unique organizational key. @param caption Shared caption.
   * @param callback Compact callback. @return Checked admission without a partial entry.
   */
  template <class Callback = detail::QuitCommand>
    requires(std::copy_constructible<Callback> && portals::detail::ManagedAction<Callback, ActionInvocation>)
  std::expected<ActionRef, TreeError> add(std::string_view key, std::string_view caption, Callback callback) {
    try {
      ActionMetadata metadata;

      metadata.caption = caption;

      return add(key, Action{std::move(metadata), std::move(callback)});
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (...) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }
  }

private:
  ActionRef failed(TreeError failure) const;
  std::shared_ptr<detail::ActionStore> store_;
};

namespace detail {
/// @cond INTERNAL
/** @brief Cold recipes/stages retain declarations until their existing managed registration takes ownership. */
template <class Callback> auto retainAction(const Callback &callback) {
  if constexpr (std::same_as<Callback, ActionRef>) {
    return ActionAccess::retain(callback);
  } else {
    return std::monostate{};
  }
}

/// @endcond
} // namespace detail
} // namespace portals::ui
