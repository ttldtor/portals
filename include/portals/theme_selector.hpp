// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/dialog.hpp>
#include <portals/theme_selection.hpp>

#include <concepts>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace portals::ui {
template <class T> class Description;
template <class T> class Ref;

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

/** @brief Advanced owning catalog settings for a selector with its scene-owned default model.
 * The compact nonempty catalog overload uses the same implementation with default normalization settings.
 */
struct ThemeSelectorCatalog {
  /** @brief Keep owning input transfers fallible with checked standard-library containers. */
  ~ThemeSelectorCatalog() = default;
  std::vector<ThemeChoice> choices;     ///< Copied synchronous catalog, normalized once before creating the view.
  ThemeSelectionOptions selection{};    ///< Existing normalization budgets, resolver and first-preview layers.
  std::optional<std::string> initialId; ///< Optional startup preview+commit after scene configuration; no callback.
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
  /** @param id Exact catalog ID, borrowed only for the existing synchronous model preview.
   * @param origin Original checked command site.
   * @return Existing model preview result without opening the view or selecting a different UI row.
   * An independently selected UI row remains protected by Apply's expected-ID guard.
   */
  std::expected<void, ManagedError> preview(std::string_view id,
                                            std::source_location origin = std::source_location::current()) const;
  /** @param origin Original checked command site.
   * @return Existing model commit result; does not close the view, call onCommitted or replace lastApply.
   * Use apply() for the selected UI choice and its once-only callback/close outcome.
   */
  std::expected<void, ManagedError> commit(std::source_location origin = std::source_location::current()) const;
  /** @return Owning current model metadata, or full checked affinity/lifetime/transition failure.
   * Requires this live attached view; retained immutable snapshots remain usable after scene retirement.
   */
  std::expected<std::shared_ptr<const ThemeSelectionSnapshot>, ManagedError> snapshot() const;
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

/** @brief Canonical typed observation of the existing scene-owned selector facade.
 * Copies and moves share/transfer the legacy weak identity without adding a slot or retaining a view/model.
 * Commands remain checked-only; lastApply retains its owning metadata under the existing retirement policy.
 * No generic node(), text() or focus() is exposed; use a separate Ref<Dialog> for the real view when needed.
 */
template <> class Ref<ThemeSelectorRef> : public ThemeSelectorRef {
public:
  /** @brief Create the existing cold selector identity; construction may throw std::bad_alloc. */
  Ref() = default;

  /** @param reference Existing weak facade, copied from lvalues or moved from rvalues without allocating.
   * The observation preserves the original identity, affinity and completed Apply metadata.
   */
  explicit Ref(ThemeSelectorRef reference) noexcept : ThemeSelectorRef(std::move(reference)) {
  }
};

/** @param reference Cold identity, published after post-configuration attachment succeeds.
 * @param model Same-scene P26 model, installed through ordinary scene configuration.
 * @param options Owning captions, modal geometry, canonical opening command and optional delivery.
 * @return Initially closed existing Dialog recipe for scene(body,layers...) or an explicit overlayHost.
 * Only one view can attach to a model; failed attachment never replaces an independently published identity.
 */
Description<Dialog> themeSelector(ThemeSelectorRef reference, ThemeSelectionRef model,
                                  ThemeSelectorOptions options = {});

/** @brief Describe one selector and its own scene-owned model without a public ThemeSelectionRef.
 * @tparam N Positive catalog size deduced from the nonempty braced array.
 * @param reference Cold view identity, published only after successful post-configuration attachment.
 * @param choices Owning choices copied during this call; factories run only on explicit preview attempts.
 * @param options Existing view captions, opening shortcut and optional Apply callback.
 * @return Initially closed Dialog recipe for scene(body,layers...) or an explicit overlayHost.
 * The existing catalog normalization and model/view uniqueness rules apply; mount failure publishes no view.
 * Empty braces retain the legacy explicit-model overload rather than selecting an empty default catalog.
 */
template <std::size_t N>
  requires(N > 0)
Description<Dialog> themeSelector(ThemeSelectorRef reference, const ThemeChoice (&choices)[N],
                                  ThemeSelectorOptions options = {});

/** @brief Configure the same default model with explicit catalog limits or an initial committed choice.
 * @tparam Catalog Exactly ThemeSelectorCatalog, optionally cv/ref-qualified; bare braces do not deduce this type.
 * @param reference Cold selector identity sharing the existing weak observation.
 * @param catalog Owning input copied now; initialId is previewed/committed after scene configuration.
 * @param options Existing view options; startup never calls onCommitted or creates a ThemeApplyOutcome.
 * @return Existing Dialog recipe; invalid/duplicate catalog or initial factory failure preserves typed errors.
 */
template <class Catalog>
  requires std::same_as<std::remove_cvref_t<Catalog>, ThemeSelectorCatalog>
Description<Dialog> themeSelector(ThemeSelectorRef reference, Catalog &&catalog, ThemeSelectorOptions options = {});
} // namespace portals::ui

namespace portals {
/** @brief Canonical composite facade type for ui::Ref<ThemeSelector>; exactly the legacy ui::ThemeSelectorRef.
 * The facade observes a view and its owning Apply metadata; it does not become a Widget or theme-selection model.
 */
using ThemeSelector = ui::ThemeSelectorRef;
} // namespace portals
