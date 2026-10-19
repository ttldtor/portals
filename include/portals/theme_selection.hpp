// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/managed_diagnostics.hpp>
#include <portals/style.hpp>
#include <portals/text_value.hpp>

#include <initializer_list>
#include <source_location>

namespace portals {
class ManagedScene;

namespace detail {
struct ThemeSelectionRecord;
}

namespace ui {
class Scene;

namespace detail {
struct ThemeSelectionTarget;
struct ThemeSelectionAccess;
struct ThemeSelectorAccess;
struct ThemeSelectionDescription;
} // namespace detail

/** @brief Logical selection session, separate from historical application choices. */
enum class ThemeSelectionStatus {
  IDLE,      ///< No installed candidate; historical lastCommittedId may still be present.
  ACTIVE,    ///< This model owns the sole effective THEME preview contribution.
  SUPERSEDED ///< Another successful theme operation retired this session; IDs are historical only.
};

/** @brief Owning normalized catalog metadata, without a factory or UI references. */
struct ThemeChoiceData {
  /** @param id Application ID copied into independent metadata.
   * @param label Validated display label copied with its receiving width policy; construction may allocate.
   */
  ThemeChoiceData(std::string_view id, const GraphemeText &label) : id(id), label(label) {
  }

  /** @brief Keep owning string/text transfers fallible with the checked standard library. */
  ~ThemeChoiceData() = default;
  std::string id;                          ///< Unique application catalog ID, never an engine token.
  GraphemeText label;                      ///< Resolved display label under the receiving scene width policy.
  std::optional<GraphemeText> description; ///< Optional resolved explanation, independently owned.
};

/** @brief Fixed owning metadata shared by independent snapshots. */
struct ThemeCatalog {
  /** @brief Keep checked vector transfers fallible. */
  ~ThemeCatalog() = default;
  std::vector<std::shared_ptr<const ThemeChoiceData>> choices =
    std::vector<std::shared_ptr<const ThemeChoiceData>>(0); ///< Ordered metadata; no factories or callbacks.
};

/** @brief Owning point-in-time read model; IDs do not claim the identity of an external theme.
 * Retained snapshots are immutable observations, not live status subscriptions.
 */
struct ThemeSelectionSnapshot {
  /** @brief Keep owning ID transfers fallible. */
  ~ThemeSelectionSnapshot() = default;
  std::shared_ptr<const ThemeCatalog> catalog;             ///< Fixed normalized catalog, usable after scene shutdown.
  ThemeSelectionStatus status{ThemeSelectionStatus::IDLE}; ///< Current preview authority status.
  std::optional<std::string> previewedId;     ///< Installed choice when ACTIVE; historical choice when SUPERSEDED.
  std::optional<std::string> lastCommittedId; ///< Historical successful application choice; absent initially.
};

/** @brief Finite normalization budgets and optional first-preview layer admission. */
struct ThemeSelectionOptions {
  /** @brief Keep owning resolver copies fallible. */
  ~ThemeSelectionOptions() = default;
  std::size_t maxChoices{64};                      ///< Positive maximum number of catalog entries.
  std::size_t maxIdBytes{128};                     ///< Positive UTF-8 byte cap for each nonempty unique ID.
  std::size_t maxLabelBytes{512};                  ///< Positive resolved label byte cap.
  std::size_t maxDescriptionBytes{2048};           ///< Positive resolved description byte cap.
  std::size_t maxTextBytes{1024 * 1024};           ///< Positive aggregate logical ID/label/description byte cap.
  TextLimits textLimits{2048, 4096, 16384, 32768}; ///< Positive per-value validation limits beneath byte caps.
  std::function<std::expected<std::string, TextResourceError>(std::string_view)> textResolver; ///< Local UI resolver.
  std::optional<StyleLayerOptions> layers; ///< Absent preserves established budgets or uses engine defaults.
};

/** @brief Owning catalog input; factories run synchronously once per explicit preview attempt on UI.
 * Factories must not block. Acquisition on workers remains application work. Borrowed captures must outlive
 * their use; the library owns its callable copy and never moves UI captures to a worker.
 */
class ThemeChoice {
public:
  /** @tparam Factory Copyable nullary callable returning StyleSheet or exactly expected<StyleSheet,StyleError>.
   * @param id Nonempty unique application ID, copied immediately.
   * @param label Owning literal/resource expression, resolved during setup.
   * @param factory Owning synchronous candidate producer; construction may allocate.
   * @param description Optional owning expression resolved during setup.
   */
  template <class Factory>
    requires std::copy_constructible<Factory> && std::invocable<Factory &> &&
               (std::same_as<std::invoke_result_t<Factory &>, StyleSheet> ||
                std::same_as<std::invoke_result_t<Factory &>, std::expected<StyleSheet, StyleError>>)
  ThemeChoice(std::string_view id, TextValue label, Factory factory, std::optional<TextValue> description = {})
      : id_(id), label_(std::move(label)), description_(std::move(description)),
        factory_([factory = std::move(factory)]() mutable -> std::expected<StyleSheet, StyleError> {
          return std::invoke(factory);
        }) {
  }

  /** @tparam Factory Same synchronous factory contract as the owning-text overload.
   * @param id Application ID copied immediately. @param label Literal bytes copied immediately.
   * @param factory Owning candidate producer. @param description Optional owning explanation.
   */
  template <class Factory>
    requires std::copy_constructible<Factory> && std::invocable<Factory &> &&
             (std::same_as<std::invoke_result_t<Factory &>, StyleSheet> ||
              std::same_as<std::invoke_result_t<Factory &>, std::expected<StyleSheet, StyleError>>)
  ThemeChoice(std::string_view id, std::string_view label, Factory factory, std::optional<TextValue> description = {})
      : ThemeChoice(id, TextValue(label), std::move(factory), std::move(description)) {
  }

  /** @brief Retain fallible ordinary copies of the owning catalog input. */
  ~ThemeChoice() = default;

private:
  friend class portals::ManagedScene;
  std::string id_;
  TextValue label_;
  std::optional<TextValue> description_;
  std::function<std::expected<StyleSheet, StyleError>()> factory_;
};

/** @brief Weak UI-affine observation of the one scene-owned catalog/model.
 * Copying/destroying observations never cancels a preview. The first successful preview explicitly revokes
 * a previous LiveTheme controller; cancel restores appearance, not that controller or legacy engine mode.
 * Commands are checked and do not report on destruction. Return them from a managed action to use its
 * existing error boundary. A failed transition preserves prior model/style state, apart from independent
 * external operations. Factories are never retried automatically.
 */
class ThemeSelectionRef {
public:
  /** @brief Construct a cold observation on the UI thread; allocation may throw before an owner exists. */
  ThemeSelectionRef();
  /** @param id Exact application catalog ID, borrowed for this synchronous operation.
   * @param origin Original command site retained in full managed errors.
   * @return Success installing one candidate or a typed error preserving the previous candidate.
   * Explicit preview after SUPERSEDED starts a fresh session from the current external theme.
   */
  std::expected<void, ManagedError> preview(std::string_view id,
                                            std::source_location origin = std::source_location::current()) const;
  /** @param origin Original command site.
   * @return Success only after promoting an ACTIVE installed candidate; IDLE/SUPERSEDED returns
   * STYLE/StyleError/INVALID_ARGUMENT and produces no new committed result. No persistence callback runs.
   */
  std::expected<void, ManagedError> commit(std::source_location origin = std::source_location::current()) const;
  /** @param origin Original command site.
   * @return Allocation-free success from IDLE, or atomic restore from ACTIVE. SUPERSEDED returns
   * STYLE/StyleError/INVALID_ARGUMENT without overwriting the external theme or historical IDs.
   */
  std::expected<void, ManagedError> cancel(std::source_location origin = std::source_location::current()) const;
  /** @return Independent owning metadata/selection, or exact affinity/lifetime/allocation failure.
   * Read-only; snapshots retain no factory, resolver, scene or widget pointer. Transition reentry is WOULD_BLOCK.
   */
  std::expected<std::shared_ptr<const ThemeSelectionSnapshot>, ManagedError> snapshot() const;

private:
  friend class portals::ManagedScene;
  friend struct detail::ThemeSelectionAccess;
  friend struct detail::ThemeSelectorAccess;
  std::shared_ptr<detail::ThemeSelectionTarget> target_;
};

/** @brief Owning deferred scene configuration; setup publishes the observation only after normalization succeeds.
 * A failed installer does not replace an existing catalog. The facade's existing configuration-error policy may
 * subsequently close the owner; independently owning catalog snapshots survive that cleanup.
 */
class ThemeSelectionStage {
public:
  /** @param reference Cold observing identity. @param catalog Owning choices copied before scene entry.
   * @param options Finite normalization/layer settings. @param origin Setup registration site.
   * Construction may allocate; the ordinary application bad_alloc boundary applies.
   */
  ThemeSelectionStage(ThemeSelectionRef reference, std::initializer_list<ThemeChoice> catalog,
                      ThemeSelectionOptions options = {},
                      std::source_location origin = std::source_location::current());
  /** @param owner Stationary scene receiving checked model setup. @param stage Owning deferred configuration.
   * @return The receiving scene; a second model setup fails INVALID_ARGUMENT without replacing the first catalog.
   */
  friend Scene &operator|(Scene &owner, const ThemeSelectionStage &stage);
  /** @param input Consumed idle scene. @param stage Owning deferred configuration.
   * @return Owning scene for the ordinary leading-pipe chain.
   */
  friend Scene operator|(Scene &&input, const ThemeSelectionStage &stage);

private:
  std::shared_ptr<const detail::ThemeSelectionDescription> description_;
  std::source_location origin_;
};

/** @param reference Cold weak observation, shared by future views without taking ownership of a session.
 * @param catalog Owning catalog with unique human IDs and synchronous factories.
 * @param options Finite normalization/layer settings. @param origin Setup registration site.
 * @return Ordinary scene configuration stage; independent scenes own independent models.
 */
ThemeSelectionStage themeSelection(ThemeSelectionRef reference, std::initializer_list<ThemeChoice> catalog,
                                   ThemeSelectionOptions options = {},
                                   std::source_location origin = std::source_location::current());
} // namespace ui
} // namespace portals
