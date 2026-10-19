// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/dialog.hpp>
#include <portals/theme_selection.hpp>

namespace portals::ui {
template <class T> class Description;

namespace detail {
struct ThemeSelectorTarget;
struct ThemeSelectorAccess;
} // namespace detail

/** @brief Actual view disposition after a committed application choice. */
enum class ThemeSelectorCloseDisposition {
  NOT_ATTEMPTED, ///< No close attempt has yet been made.
  OPEN,          ///< Close failed before publication; the view remains open and retryable.
  CLOSED,        ///< Ordinary close published, including a later notification failure.
  RETIRED        ///< The original view or scene was retired; no stale UI access was attempted.
};

/** @brief One immutable owning completed Apply result, independent of the view lifetime.
 * Both error slots preserve the complete original envelope. A callback failure never undoes a successful commit.
 */
struct ThemeApplyOutcome {
  /** @brief Retain fallible owning ID copies with checked standard-library configurations. */
  ~ThemeApplyOutcome() = default;
  std::string committedId = std::string(0, '\0'); ///< Successfully committed application catalog ID.
  ThemeSelectorCloseDisposition closeDisposition{ThemeSelectorCloseDisposition::NOT_ATTEMPTED}; ///< Actual final state.
  std::optional<ManagedError> callbackError; ///< Original optional persistence callback failure.
  std::optional<ManagedError> closeError;    ///< Independent full view-close failure; never replaces callback cleanup.
};

/** @brief Owning selector captions, established dialog settings and optional application delivery. */
struct ThemeSelectorOptions {
  /** @brief Keep owning option/callback transfers fallible. */
  ~ThemeSelectorOptions() = default;
  DialogOptions dialog{};     ///< Existing modal geometry defaults; eager Escape is disabled by this composition.
  TextValue title{"Themes"};  ///< Owning dialog title, resolved during mounting.
  TextValue apply{"Apply"};   ///< Owning Apply caption.
  TextValue cancel{"Cancel"}; ///< Owning checked-restoration caption.
  TextValue closeKeepingPreview{"Close (keep preview)"}; ///< Explicit hide-only recovery action.
  TextValue emptyStatus{"Choose a theme to preview"};    ///< No currently selected active choice.
  TextValue errorStatus{"Theme action failed; retry or close keeping preview"}; ///< Bounded recoverable status.
  TextValue conflictStatus{"Theme changed elsewhere; preview again or close keeping preview"}; ///< Superseded status.
  TextLimits textLimits{4096, 4096, 4096, 8192}; ///< Positive validation limits for each caption/status.
  std::function<std::expected<std::string, TextResourceError>(std::string_view)>
    textResolver; ///< Local mount resolver.
  std::optional<std::string> openingShortcut{std::in_place,
                                             "Ctrl+K, Ctrl+T"}; ///< Canonical text; absent disables binding.
  /** @brief Optional synchronous UI callback, invoked once after a successful guarded commit.
   * The ID borrow expires on return; copy it for later use. No file I/O is supplied by the library.
   * Failure does not skip close or repeat delivery. It must not block.
   */
  std::function<std::expected<void, ManagedError>(std::string_view)> onCommitted;
};

/** @brief Weak UI-affine observation of one scene-owned selector view.
 * Commands are checked only: they never report, remember or stop the application. Built-in controls use report-only
 * diagnostics separately. Copy/destruction never restores a theme. An active model may outlive a retired view.
 */
class ThemeSelectorRef {
public:
  /** @brief Create a cold identity; allocation may throw before a scene owner exists. */
  ThemeSelectorRef();
  /** @param origin Original checked command site. @return Open/refocus without invoking a factory, or full failure. */
  std::expected<void, ManagedError> open(std::source_location origin = std::source_location::current()) const;
  /** @param origin Original checked command site.
   * @return Guarded commit, once-only callback and close; callback error takes precedence over close error.
   * Precommit rejection preserves lastApply. Reentry during transition is WOULD_BLOCK.
   */
  std::expected<void, ManagedError> apply(std::source_location origin = std::source_location::current()) const;
  /** @param origin Original checked command site. @return Checked restoration before close; failure leaves view usable.
   */
  std::expected<void, ManagedError> cancel(std::source_location origin = std::source_location::current()) const;
  /** @param origin Original checked command site. @return Hide only, retaining the model preview/session. */
  std::expected<void, ManagedError>
  closeKeepingPreview(std::source_location origin = std::source_location::current()) const;
  /** @return Allocation-free owning completed result; null success before any committed Apply.
   * UI-affine metadata remains readable after view/scene retirement. During transition the previous result survives.
   * This query owns no scene, widget or callback.
   */
  std::expected<std::shared_ptr<const ThemeApplyOutcome>, ManagedError> lastApply() const;

private:
  friend struct detail::ThemeSelectorAccess;
  std::shared_ptr<detail::ThemeSelectorTarget> target_;
};

/** @param reference Cold identity, published after post-configuration attachment succeeds.
 * @param model Same-scene P26 model, installed through ordinary scene configuration.
 * @param options Owning captions, modal geometry, canonical opening command and optional delivery.
 * @return Initially closed existing Dialog recipe, requiring an enclosing overlayHost.
 * Only one view can attach to a model; failed attachment never replaces an independently published identity.
 */
Description<Dialog> themeSelector(ThemeSelectorRef reference, ThemeSelectionRef model,
                                  ThemeSelectorOptions options = {});
} // namespace portals::ui
