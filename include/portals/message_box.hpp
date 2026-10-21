// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/async.hpp>
#include <portals/ui_ref.hpp>

// Keep the exported facade name stable regardless of Windows header order, then restore the caller's macro.
#pragma push_macro("MessageBox")
#undef MessageBox

namespace portals {
/** @brief A requested application action; SAVE does not confirm that data was written. */
enum class MessageChoice {
  ACKNOWLEDGE, ///< Acknowledge an informational message.
  CONFIRM,     ///< Proceed with the described operation.
  SAVE,        ///< Request saving before proceeding.
  DISCARD,     ///< Proceed without saving.
  CANCEL       ///< User/programmatic cancellation, never an owner or task failure.
};
/** @brief Immutable domain outcome, preserving the complete original managed failure. */
using MessageResult = std::expected<MessageChoice, ManagedError>;

namespace ui {
template <class T> class Ref;
template <class T> class Description;
class MessageBox;

/** @brief Owning localizable button captions, resolved once during complete mount. */
struct MessageLabels {
  TextValue acknowledge{"OK"};   ///< Information action.
  TextValue confirm{"Continue"}; ///< Confirmation action.
  TextValue save{"Save"};        ///< Save request.
  TextValue discard{"Discard"};  ///< Discard request.
  TextValue cancel{"Cancel"};    ///< Safe cancellation action.
};

/** @brief Immutable settings for a ready-made modal choice dialog. */
struct MessageOptions {
  DialogOptions dialog{};  ///< Existing panel settings; MODAL and closeOnEscape are required.
  MessageLabels labels{};  ///< Owning captions; empty or multiline captions are rejected.
  TextResolver resolver{}; ///< Explicit synchronous localization; never retained by mounted widgets.
  std::optional<MessageChoice> defaultChoice{};     ///< Absent selects ACKNOWLEDGE or the safe CANCEL action.
  bool allowDestructiveDefault{};                   ///< Explicit permission for CONFIRM/DISCARD initial focus.
  TextLimits textLimits{16384, 4096, 16384, 32768}; ///< Positive per-value UTF-8/cluster/grapheme/cell caps.
  std::size_t maxBodyLines{4096};                   ///< Positive wrapped explanation line cap; excess is NODE_LIMIT.
};

namespace detail {
/// @cond INTERNAL
struct MessageTarget : RefTarget {
  bool pending{};
};

struct MessageAccess {
  template <class T> static std::shared_ptr<RefTarget> target(const Ref<T> &ref) {
    return ref.target();
  }

  static DialogCommandResult command(std::shared_ptr<RefTarget> target, std::expected<void, ManagedError> result,
                                     std::source_location origin);
  static std::expected<void, ManagedError> start(ManagedScene &scene, Node<Widget> dialog, Node<Widget> owner,
                                                 std::function<EditorCommandResult(const MessageResult &)> callback,
                                                 std::source_location origin);
  static std::expected<void, ManagedError> cancel(ManagedScene &scene, Node<Widget> dialog);
  static std::expected<std::shared_ptr<const MessageResult>, ManagedError> last(ManagedScene &scene,
                                                                                Node<Widget> dialog);
  static std::expected<AsyncReceive<MessageResult>, ManagedError>
  receive(ManagedScene &scene, Node<Widget> dialog, Node<Widget> owner, const UiAsyncContext &context);
  static Description<Dialog> describe(MessageBox box, TextValue title, TextValue body, MessageOptions options,
                                      int kind);
  static std::expected<void, ManagedError> validate(ManagedScene &scene, Node<Widget> dialog, Node<Widget> owner);
  static std::expected<std::shared_ptr<portals::detail::MessageRequest>, ManagedError>
  reserve(ManagedScene &scene, Node<Widget> dialog, Node<Widget> owner);

  template <class Callback> static auto adapt(ManagedScene &scene, Callback callback) {
    return scene.actionAdapter<MessageResult>(std::move(callback));
  }
};

/// @endcond
} // namespace detail

/** @brief Copyable UI-affine observation of a reusable choice dialog.
 * Copies retain no scene/widget/callback. Cold handles return CLOSED; original removed identities
 * return STALE_NODE. A request remains busy through deferred callback admission or async retirement.
 */
class MessageBox {
public:
  /** @brief Create an unpublished observing slot; allocation can throw. */
  MessageBox();

  /** @tparam Callback Existing compact managed action accepting const MessageResult& or no arguments.
   * @param callback Owning captures admitted once on a later Scheduler DATA dispatch.
   * @param origin Public command site retained by diagnostics.
   * @return Checked admission; no callback is entered for a rejected request. Default owner is scene.root().
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, MessageResult>
  DialogCommandResult open(Callback callback, std::source_location origin = std::source_location::current()) const {
    return openTarget({}, std::move(callback), origin);
  }

  /** @tparam T Explicit owner widget type. @tparam Callback Existing compact managed action.
   * @param owner Live ordinary-body or active-modal owner in the same scene.
   * @param callback Owning deferred action; the argument is borrowed only during admission.
   * @param origin Public command site. @return Checked admission, preserving old result on failure.
   */
  template <class T, class Callback>
    requires std::derived_from<T, Widget> && portals::detail::ManagedAction<Callback, MessageResult>
  DialogCommandResult open(const Ref<T> &owner, Callback callback,
                           std::source_location origin = std::source_location::current()) const {
    return openTarget(detail::MessageAccess::target(owner), std::move(callback), origin);
  }

  /** @param origin Command site. @return Checked CANCEL dismissal; a closed panel is an equal no-op.
   * Explicit cleanup of a surviving failed-open panel preserves its already latched error.
   */
  DialogCommandResult cancel(std::source_location origin = std::source_location::current()) const;
  /** @return Owning immutable snapshot, WOULD_BLOCK before latch, or exact facade liveness/affinity failure.
   * Once latched, task cancellation and transport pressure never replace its pointer or value.
   */
  std::expected<std::shared_ptr<const MessageResult>, ManagedError> lastResult() const;
  /** @param context Same-scene existing UiAsync context. @return Owning reservation; opens only at actual await.
   * The ordinary AsyncReceive outer error retains transport errors; its MessageResult retains full domain errors.
   * Reservations, copied awaiters and their final release stay on the constructing UI thread.
   */
  std::expected<AsyncReceive<MessageResult>, ManagedError> receive(const UiAsyncContext &context) const;

  /** @tparam T Explicit owner type. @param context Same-scene receiver. @param owner Independent live owner.
   * @return Checked owning reservation; owner removal can deliver a full error to an independent receiver.
   */
  template <class T>
    requires std::derived_from<T, Widget>
  std::expected<AsyncReceive<MessageResult>, ManagedError> receive(const UiAsyncContext &context,
                                                                   const Ref<T> &owner) const {
    return receiveTarget(context, detail::MessageAccess::target(owner));
  }

private:
  friend struct detail::MessageAccess;

  template <class Callback>
  DialogCommandResult openTarget(std::shared_ptr<detail::RefTarget> owner, Callback callback,
                                 std::source_location origin) const {
    auto scene = detail::eventScene(target_);
    if (!scene) {
      return detail::MessageAccess::command(target_, std::unexpected(scene.error()), origin);
    }
    Node<Widget> node = (*scene)->root();
    if (owner) {
      auto ownerScene = detail::eventScene(owner);
      if (!ownerScene) {
        return detail::MessageAccess::command(target_, std::unexpected(ownerScene.error()), origin);
      }
      if (*ownerScene != *scene) {
        return detail::MessageAccess::command(
          target_,
          std::unexpected(ManagedError{ManagedPhase::INPUT, owner->node.id(), TreeError::WRONG_SESSION, {}, {}}),
          origin);
      }
      node = owner->node;
    }
    try {
      return detail::MessageAccess::command(
        target_,
        detail::MessageAccess::start(**scene, target_->node, node,
                                     detail::MessageAccess::adapt(**scene, std::move(callback)), origin),
        origin);
    } catch (const std::bad_alloc &) {
      return detail::MessageAccess::command(
        target_, std::unexpected(ManagedError{ManagedPhase::INPUT, target_->node.id(), TreeError::NO_MEMORY, {}, {}}),
        origin);
    }
  }

  std::expected<AsyncReceive<MessageResult>, ManagedError>
  receiveTarget(const UiAsyncContext &context, std::shared_ptr<detail::RefTarget> owner) const;
  std::shared_ptr<detail::MessageTarget> target_;
};

#ifdef _WIN32
/** @brief Preserve qualified legacy ui::MessageBox use when Win32 selects its ANSI spelling. */
using MessageBoxA = MessageBox;
/** @brief Preserve qualified legacy ui::MessageBox use when Win32 selects its Unicode spelling. */
using MessageBoxW = MessageBox;
#endif

/** @brief Canonical typed observation of the existing reusable message-dialog facade.
 * Copies and moves share/transfer the legacy weak identity without adding a slot or retaining the scene.
 * All commands preserve MessageBox's checked results and report-once provenance. This composite reference
 * has no generic node(), text() or focus(); bind a separate Ref<Dialog> to observe its real view.
 */
template <> class Ref<MessageBox> : public MessageBox {
public:
  /** @brief Create the existing cold observing slot; construction may throw std::bad_alloc. */
  Ref() = default;

  /** @param reference Existing weak facade, copied from lvalues or moved from rvalues without allocating.
   * The new observation retains the same original identity and affinity, including after retirement.
   */
  explicit Ref(MessageBox reference) noexcept : MessageBox(std::move(reference)) {
  }
};

/** @param handle Cold observing handle. @param title Owning localizable heading.
 * @param explanation Owning immutable wrapped explanation. @param options Modal settings.
 * @return Exact initially hidden Dialog description with ACKNOWLEDGE.
 */
Description<Dialog> messageBox(MessageBox handle, TextValue title, TextValue explanation, MessageOptions options = {});
/** @param handle Cold observation. @param title Heading. @param explanation Explanation. @param options Settings.
 * @return Exact Dialog description with CONFIRM/CANCEL and safe CANCEL initial focus.
 */
Description<Dialog> confirmDialog(MessageBox handle, TextValue title, TextValue explanation,
                                  MessageOptions options = {});
/** @param handle Cold observation. @param title Heading. @param explanation Explanation. @param options Settings.
 * @return Exact Dialog description with SAVE/DISCARD/CANCEL and safe CANCEL initial focus.
 */
Description<Dialog> unsavedChangesDialog(MessageBox handle, TextValue title, TextValue explanation,
                                         MessageOptions options = {});
/** @param handle Cold observation. @param title Copied UTF-8 heading. @param explanation Copied UTF-8 explanation.
 * @param options Modal settings. @return Information recipe; string views are copied immediately.
 */
Description<Dialog> messageBox(MessageBox handle, std::string_view title, std::string_view explanation,
                               MessageOptions options = {});
/** @param handle Cold observation. @param title Copied heading. @param explanation Copied explanation.
 * @param options Modal settings. @return Confirmation recipe.
 */
Description<Dialog> confirmDialog(MessageBox handle, std::string_view title, std::string_view explanation,
                                  MessageOptions options = {});
/** @param handle Cold observation. @param title Copied heading. @param explanation Copied explanation.
 * @param options Modal settings. @return Unsaved-changes recipe.
 */
Description<Dialog> unsavedChangesDialog(MessageBox handle, std::string_view title, std::string_view explanation,
                                         MessageOptions options = {});
} // namespace ui

/** @brief Canonical composite type for ui::Ref<MessageDialog>; exactly the existing ui::MessageBox facade.
 * The name avoids the Win32 MessageBox macro. Existing qualified handles and factory signatures remain valid.
 */
using MessageDialog = ui::MessageBox;
} // namespace portals

#pragma pop_macro("MessageBox")
