// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/dialog_command_result.hpp>
#include <portals/detail/quit_command.hpp>
#include <portals/find_bar.hpp>
#include <portals/form_field.hpp>
#include <portals/help_view.hpp>
#include <portals/managed.hpp>
#include <portals/text_value.hpp>

#include <typeinfo>

namespace portals::ui {
template <class T> class Description;
template <class T> class Ref;
class DynamicRef;
class StyleStage;
class DialogCommandResult;

namespace detail {
struct SceneAnchor;
struct RefTarget;

/** @brief Owning request identity; the facade and its widgets remain weakly observed. */
struct LookupSource {
  /** @brief Suppress implicit noexcept moves; checked MSVC string transfers can allocate. */
  ~LookupSource() = default;

  std::weak_ptr<SceneAnchor> anchor;        ///< Stable facade publication, revoked before cleanup.
  std::thread::id owner;                    ///< Immutable originating UI thread.
  std::string name = std::string(0, '\0');  ///< Case-sensitive ID; fallible default construction stays checked.
  std::source_location origin;              ///< Original lookup site.
  const char *expectedType{"Widget"};       ///< Static RTTI label for typed diagnostic context.
  std::shared_ptr<RefTarget> observation{}; ///< Optional original dynamic slot for identity-preserving conversion.
};

/** @brief Owning publication callback; invoked synchronously after checked name resolution. */
using LookupPublisher =
  std::function<std::expected<void, ManagedError>(ManagedScene &, std::expected<Node<Widget>, ManagedError>)>;

/** @param source Owning name and weak facade observation.
 * @param publish Owning slot publication callback, transferred before deferred execution.
 * @return Success for a queued/published request or the exact admission error.
 */
std::expected<void, ManagedError> requestLookup(const LookupSource &source, LookupPublisher publish);

struct NamedAccess;

/** @brief Shared observation; publication and all mutable fields belong to the constructing UI thread. */
struct RefTarget {
  const std::thread::id owner = std::this_thread::get_id(); ///< Immutable affinity checked before mutable fields.
  ManagedScene *scene{};                                    ///< Borrowed only while lifetime remains live on UI.
  std::weak_ptr<const void> lifetime;                       ///< Does not retain the scene or its widgets.
  Node<Widget> node;                                        ///< Published identity, never automatically reset.
  Node<Control> caption;                                    ///< Typed caption identity, empty for other widgets.
  std::shared_ptr<const ManagedError> failure; ///< Owning failure, allocated only for a rejected named request.
  Node<LineEdit> draft;                        ///< Typed editor identity, empty for other widgets.
  Node<Password> secret;                       ///< Protected editor identity; never grants plaintext access.
};

struct RefAccess;
/** @param target Weak observation checked for UI affinity and scene lifetime.
 * @param node Shared typed slot, borrowed only after the affinity check.
 * @param value Finite replacement or delta. @param relative Add to the current value without clamping.
 * @param origin Original command site. @return Checked mutation; failures enter the existing collector once.
 */
std::expected<void, ManagedError> gaugeValue(std::shared_ptr<RefTarget> target, std::shared_ptr<const Node<Gauge>> node,
                                             double value, bool relative, std::source_location origin);
std::expected<FieldValidation, ManagedError> fieldValidation(const std::shared_ptr<RefTarget> &target);
Ref<LineEdit> fieldEditor(const std::shared_ptr<RefTarget> &target);
std::expected<bool, ManagedError> validateField(const std::shared_ptr<RefTarget> &target);
std::expected<void, ManagedError> displayField(const std::shared_ptr<RefTarget> &target, FieldValidation value);
Ref<LineEdit> findEditor(const std::shared_ptr<RefTarget> &target);
std::expected<FindMatch, ManagedError> findNavigate(const std::shared_ptr<RefTarget> &target, FindDirection direction);
std::expected<FindMatch, ManagedError> findMatch(const std::shared_ptr<RefTarget> &target);
std::expected<bool, ManagedError> findCurrent(const std::shared_ptr<RefTarget> &target, const FindMatch &match);
std::expected<void, ManagedError> findDocument(const std::shared_ptr<RefTarget> &target, std::string_view document);
EditorCommandResult findClear(const std::shared_ptr<RefTarget> &target, std::source_location origin);
std::pair<std::expected<void, TreeError>, bool> clearFindEditor(ManagedScene &scene, Node<Widget> node,
                                                                std::source_location origin);
struct MessageAccess;
template <class> struct TableRowType;

/** @brief Domain row type associated with a typed virtual table reference. */
template <class T> struct TableRowType<VirtualTable<T>> {
  using Type = T; ///< Domain value retained by each model row.
};

/** @param target Weak checked observation. @param value Owning explicit state.
 * @param origin Public mutation call site. @return Original TreeError, reported once through the live collector.
 */
std::expected<void, TreeError> displayDataState(std::shared_ptr<RefTarget> target, DataPresentation value,
                                                std::source_location origin);

/** @brief Checked bridge preserving immutable Ref affinity before reading its mutable published target.
 * @param target Owning observation with weak scene lifetime; may be empty.
 * @param redo Select Redo instead of Undo when stop is false.
 * @param stop Select a grouping boundary instead of a history restoration.
 * @param origin Public command call site retained in diagnostics.
 * @param clear Select user Clear instead of a history restoration.
 * @return Owning checked result with exactly its own reporting receipt.
 */
EditorCommandResult editorCommand(std::shared_ptr<RefTarget> target, bool redo, bool stop, std::source_location origin,
                                  bool clear = false);
/** @param target Owning observing slot, without scene/widget ownership.
 * @return Checked effective availability without reporting, adoption or invalidation.
 */

std::expected<HistoryAvailability, TreeError> editorAvailability(std::shared_ptr<RefTarget> target);

/** @param target Observing slot checked for UI affinity and live scene ownership.
 * @return Live managed scene or typed BIND error; never reads mutable target fields from a foreign thread.
 */
std::expected<ManagedScene *, ManagedError> eventScene(const std::shared_ptr<RefTarget> &target);

/** @param target Checked weak protected-editor observation. @param origin Original call site.
 * @return Owning protected draft identity, with short failures collected once.
 */
std::expected<PasswordValue, ManagedError> passwordValue(std::shared_ptr<RefTarget> target,
                                                         std::source_location origin);
/** @param target Checked weak protected-editor observation. @param value Protected candidate.
 * @param origin Original call site. @return Checked native edit under the field's publication mode.
 */
std::expected<void, ManagedError> setPasswordValue(std::shared_ptr<RefTarget> target, PasswordValue value,
                                                   std::source_location origin);
/** @param target Weak original observation. @param failure Owning original protected failure.
 * @param origin Original call site. Report through the existing collector without duplicate receipts.
 */
void reportPasswordError(const std::shared_ptr<RefTarget> &target, ManagedError &failure, std::source_location origin);

/** @param target Observation checked for immutable affinity before mutable owner access.
 * @return Owning final arranged geometry or exact checked failure; never performs layout or reports.
 */
std::expected<NodeGeometry, ManagedError> geometry(const std::shared_ptr<RefTarget> &target);

/** @param target Weak published area/bar observation checked for immutable UI affinity.
 * @param axis Selected logical axis.
 * @param offset Exact logical position.
 * @param origin Command site retained by the existing error collector.
 * @return Checked command result; failures are also reported to a surviving scene collector.
 */
std::expected<void, ManagedError> scrollOffset(const std::shared_ptr<RefTarget> &target, ScrollAxis axis,
                                               ScrollOffset offset, std::source_location origin);
/** @param target Weak published descendant observation.
 * @param origin Request site retained by the existing collector.
 * @return Checked one-shot reveal admission or reported failure; no synchronous layout occurs.
 */
std::expected<void, ManagedError> ensureVisible(const std::shared_ptr<RefTarget> &target, std::source_location origin);

/** @param target Weak scene context; destroyed/foreign-thread scenes are never dereferenced.
 * @param error Exact registration error reported to a live UI collector without stopping run.
 * @param origin Ref registration call site.
 */
void reportEventError(const std::shared_ptr<RefTarget> &target, const ManagedError &error, std::source_location origin);

/** @brief Report an unreported checked focus failure, preserving its original editor collector.
 * @param target Weak destination scene context checked for immutable UI affinity and lifetime.
 * @param error Exact focus result; a copied opaque delivery receipt prevents a second report.
 * @param origin Original Ref focus request call site.
 */
void reportFocusError(const std::shared_ptr<RefTarget> &target, const ManagedError &error, std::source_location origin);

/** @brief Execute a checked short dialog command through the existing UI error collector.
 * @param target Weak exact-node observation, checked for affinity before mutable access.
 * @param opening Select open instead of dismiss.
 * @param origin Command call site retained with its report-once receipt.
 * @return Checked result; dead/foreign owners are never reported through a collector.
 */
DialogCommandResult dialogCommand(const std::shared_ptr<RefTarget> &target, bool opening, std::source_location origin);

} // namespace detail

/** @brief Explicit owning synchronous localization adapter; no global locale or resource storage. */
using TextResolver = std::function<std::expected<std::string, TextResourceError>(std::string_view)>;

namespace detail {
/** @param target Original weak observation, checked for immutable affinity before mutable slot access.
 * @param node Aliasing slot owner; node value is read only after affinity admission.
 * @param active Start or stop selection; no new runtime is created.
 * @param origin Public command call site.
 * @return Checked result, with owned failures collected by the existing short-error path.
 */
std::expected<void, ManagedError> spinnerActive(std::shared_ptr<RefTarget> target,
                                                std::shared_ptr<const Node<Spinner>> node, bool active,
                                                std::source_location origin);
/** @param target Original weak observation with immutable UI affinity.
 * @param node Aliasing typed identity, read only after admission.
 * @param value Borrowed literal bytes copied during the checked call.
 * @param origin Public command call site retained by the existing collector.
 * @return Checked label replacement; resets frame zero and restarts one interval on ordinary preparation.
 */
std::expected<void, ManagedError> spinnerLabel(std::shared_ptr<RefTarget> target,
                                               std::shared_ptr<const Node<Spinner>> node, std::string_view value,
                                               std::source_location origin);
/** @param target Original weak observation.
 * @param node Aliasing typed identity, read only after UI affinity admission.
 * @param value Borrowed owning expression, staged before publication.
 * @param resolver Explicit synchronous localization adapter, never retained after this call.
 * @param origin Public command call site retained in diagnostics.
 * @return Checked staged replacement with original resource/text error; frame zero restarts on success.
 */
std::expected<void, ManagedError> spinnerLabel(std::shared_ptr<RefTarget> target,
                                               std::shared_ptr<const Node<Spinner>> node, const TextValue &value,
                                               const TextResolver &resolver, std::source_location origin);
} // namespace detail

/** @brief Copyable read-only source observing one caption, visible draft or accepted editor model.
 * Construction and copying do not read, subscribe or retain the scene/widget. Operations are UI-affine;
 * an unpublished source returns CLOSED, and removal preserves the original stale node identity.
 * Caption factories can attach a live observation; snapshot() remains independent. No remount retargeting occurs.
 */
class TextSource {
public:
  /** @return Owning text snapshot or the original typed availability/text failure, without reporting.
   * Accepted editor values are read from the latest model, even while a dirty/conflicted draft differs.
   * Model bytes are validated under the editor's width policy and limits; malformed values remain errors.
   */
  std::expected<GraphemeText, ManagedError> snapshot() const;

private:
  template <class T> friend class Ref;
  friend class portals::ManagedScene;
  friend struct detail::Access;
  friend class TextProperty;
  enum class Kind {
    CAPTION,
    DRAFT,
    COMMITTED
  };

  TextSource(std::shared_ptr<detail::RefTarget> target, Kind kind) : target_(std::move(target)), kind_(kind) {
  }

  std::shared_ptr<detail::RefTarget> target_;
  Kind kind_;
};

class TextProperty;

/** @brief Copyable deferred observation of one original table, without scene/widget ownership.
 * @tparam T Immutable domain row type. Acquisition/copying never reads, subscribes or performs layout.
 * snapshot() is UI-affine and errors preserve original node identity. Saved snapshots are independent.
 */
template <class T> class VisibleModelSource {
public:
  /** @return Stable ready range or exact typed access/preparation/provider failure, without reporting. */
  std::expected<VisibleModelSnapshot<T>, VisibleModelError> snapshot() const {
    auto owner = detail::eventScene(target_);

    if (!owner) {
      return std::unexpected(owner.error());
    }

    return (*owner)->visibleModelSnapshot(*node_);
  }

private:
  template <class> friend class Ref;

  VisibleModelSource(std::shared_ptr<detail::RefTarget> target, std::shared_ptr<const Node<VirtualTable<T>>> node)
      : target_(std::move(target)), node_(node) {
  }

  std::shared_ptr<detail::RefTarget> target_;
  std::shared_ptr<const Node<VirtualTable<T>>> node_;
};

/** @brief Owning read-only concatenation, including the original typed failure when reading fails.
 * Successful results validate complete joined graphemes under the leftmost property/result policy
 * and default TextLimits. Sources, selections, drafts and invalidation are never changed.
 * Assignment revalidates under the receiving widget's policy and limits. No locale is inferred;
 * a resource operand uses the leftmost proxy's explicitly captured resolver.
 * Copying successful text or captured failures can allocate and throw before a receiving call starts.
 */
class TextResult {
public:
  /** @return Owning validated text copy or original read/resolve/concatenation failure.
   * Copying successful text may throw std::bad_alloc; the stored result remains unchanged.
   */
  std::expected<GraphemeText, ManagedError> result() const;

  /** @param value Property read synchronously on its UI thread without mutation.
   * @return Owning concatenation, or the first unchanged typed failure.
   */
  TextResult operator+(const TextProperty &value) const;

  /** @param value Literal suffix borrowed only for this call.
   * @return Owning concatenation with complete boundary resegmentation, or typed failure.
   */
  TextResult operator+(std::string_view value) const;

  /** @param value Owning expression; resource operands use the explicitly captured resolver.
   * @return Owning concatenation or original typed failure, without reporting to a scene.
   */
  TextResult operator+(const TextValue &value) const;

  /** @param value Another owning concatenation, borrowed only for this call.
   * @return Owning concatenation under this result's policy, or the first typed failure.
   */
  TextResult operator+(const TextResult &value) const;

private:
  friend class TextProperty;
  friend struct detail::RefAccess;
  TextResult(std::expected<GraphemeText, ManagedError> value, std::shared_ptr<TextResolver> resolver,
             std::source_location origin);
  TextResult join(std::string_view bytes) const;

  std::expected<GraphemeText, ManagedError> value_;
  std::shared_ptr<TextResolver> resolver_;
  std::source_location origin_;
};

/** @brief Copyable observing text property; copies own their resolver and last-operation result.
 * Mutation is UI-affine, outside frames/preparation and nested text mutations. Caption changes use
 * setControlText; editor changes accept a draft and apply its configured EditMode confirmation rules.
 * Every rejected mutation preserves its target value and reports to the live scene collector on UI.
 * WRONG_THREAD and an absent/destroyed scene are observable through result(), without unsafe reporting.
 * Within an entered UI setup scope, even an unavailable target failure stops that setup after the callback
 * returns. It never notifies a foreign owner; outside setup, existing runtime recovery remains unchanged.
 * Each successful operation stands independently; a later failure does not roll back earlier changes.
 * Methods return an owning proxy value so chains and saved results never borrow a temporary proxy.
 * Resolver capture construction can throw before entry. Internally allocation/extension exceptions are
 * translated to typed errors. Copy/move/assignment of this proxy may allocate in standard error storage.
 */
class TextProperty {
public:
  /** @return Deferred visible-text identity for a caption factory; does not read or subscribe.
   * Assignment between TextProperty values still copies once rather than creating a binding.
   */
  TextSource source() const {
    return TextSource(target_,
                      (expectedType_ == "LineEdit" || expectedType_ == "FormField" || expectedType_ == "FindBar")
                        ? TextSource::Kind::DRAFT
                        : TextSource::Kind::CAPTION);
  }

  /** @param other Observing proxy copied without reading or changing a widget; errors/resolver are copied too. */
  TextProperty(const TextProperty &other) = default;

  /** @param other Proxy transferred without reading or changing a widget; its source becomes empty. */
  TextProperty(TextProperty &&other) = default;

  /** @param value Property read into an owning snapshot before replacing this proxy's target value.
   * @return Owning proxy after value assignment; copying observations uses the copy constructor instead.
   */
  TextProperty operator=(const TextProperty &value);

  /** @brief Copy validated UTF-8 text out of the live widget without retaining its storage.
   * @return Owning bytes, or typed affinity/liveness/reentry/allocation error; does not report errors.
   */
  std::expected<std::string, ManagedError> get() const;

  /** @brief Inspect the last mutation of this particular proxy; newly created proxies have success.
   * @return Success or exact text-acceptance failure; no scene/widget access is performed.
   * Copies preserve internal short-report provenance for same-collector configure/setup deduplication.
   * Editor validation/publication outcomes use its LineEditResult callback, including accepted invalid drafts.
   */
  std::expected<void, ManagedError> result() const;

  /** @param value Bytes copied during this call; validation uses the receiving widget's width policy/limits.
   * @param origin Mutation call site retained in diagnostics.
   * @return Owning proxy for chaining, with this operation's result. Equal text is a no-op.
   */
  TextProperty set(std::string_view value, std::source_location origin = std::source_location::current());

  /** @param value Owning literal/format/key expression copied before invoking the optional resolver.
   * @param origin Mutation call site retained in diagnostics.
   * @return Owning proxy for chaining; missing resolver or invalid resolved text preserves the old value.
   */
  TextProperty set(const TextValue &value, std::source_location origin = std::source_location::current());

  /** @param value Bytes appended to the complete current caption/draft, without selection replacement.
   * @param origin Mutation call site retained in diagnostics.
   * @return Owning proxy; the entire candidate is resegmented and validated before mutation.
   */
  TextProperty append(std::string_view value, std::source_location origin = std::source_location::current());

  /** @param value Owning literal/format/key expression, resolved once synchronously when required.
   * @param origin Mutation call site retained in diagnostics.
   * @return Owning proxy with the exact result; failure preserves the complete old caption/draft.
   */
  TextProperty append(const TextValue &value, std::source_location origin = std::source_location::current());

  /** @param value Literal replacement, copied only during this call.
   * @return Owning proxy with the same contract as set; diagnostics use the text() call site.
   */
  TextProperty operator=(std::string_view value);

  /** @param value Owning replacement expression.
   * @return Owning proxy with the same contract as set; diagnostics use the text() call site.
   */
  TextProperty operator=(const TextValue &value);

  /** @param value Literal suffix, copied only during this call.
   * @return Owning proxy with the same contract as append; diagnostics use the text() call site.
   */
  TextProperty operator+=(std::string_view value);

  /** @param value Owning suffix expression.
   * @return Owning proxy with the same contract as append; diagnostics use the text() call site.
   */
  TextProperty operator+=(const TextValue &value);

  /** @param value Owning concatenation; a failed source preserves its typed cause and original node identity.
   * @return Owning proxy after replacement; the receiver revalidates successful bytes with its own policy/limits.
   */
  TextProperty operator=(const TextResult &value);

  /** @param value Owning concatenation, whose successful bytes are appended and revalidated by the receiver.
   * @return Owning proxy; source errors preserve the target and are reported with the concatenation origin.
   */
  TextProperty operator+=(const TextResult &value);

  /** @param value Second property read synchronously without changing either source.
   * @return Owning result under this property's width policy and default TextLimits, or original typed error.
   */
  TextResult operator+(const TextProperty &value) const;

  /** @param value Literal suffix copied now, with full boundary resegmentation.
   * @return Owning result or typed failure; no mutation, invalidation or automatic reporting occurs.
   */
  TextResult operator+(std::string_view value) const;

  /** @param value Owning expression resolved with this proxy's explicit resolver when required.
   * @return Owning result or typed failure; no source mutation occurs.
   */
  TextResult operator+(const TextValue &value) const;

  /** @param value Owning concatenation, read only during this call.
   * @return Owning result under this property's policy, or the first unchanged typed error.
   */
  TextResult operator+(const TextResult &value) const;

private:
  template <class T> friend class Ref;
  friend class DynamicRef;
  friend struct detail::RefAccess;
  friend class TextResult;

  TextProperty(std::shared_ptr<detail::RefTarget> target, TextResolver resolver, std::string_view expectedType,

               std::source_location origin, std::optional<ProgrammaticChange> change = {});

  TextProperty mutate(const TextValue &value, bool append, std::source_location origin);
  TextProperty mutate(std::string_view value, bool append, std::source_location origin);
  TextProperty mutate(const TextResult &value, bool append);
  TextResult readResult() const;
  void report(const char *operation, std::string_view key, std::source_location origin);

  std::shared_ptr<detail::RefTarget> target_;
  std::shared_ptr<TextResolver> resolver_;
  std::string_view expectedType_;
  std::source_location origin_;
  std::expected<void, ManagedError> result_;
  std::optional<ProgrammaticChange> change_;
};

/** @brief Shared observing slot published only after a complete successful description mount.
 * @tparam T Observed public Widget subtype; no widget or scene ownership is retained.
 * Read/write the slot only on the UI thread. Copies share the slot. A published identity is
 * never automatically reset; checked tree access detects later retirement. Use a new slot
 * for another description. Construction can throw std::bad_alloc.
 */
template <class T> class NodeRef {
public:
  /** @brief Allocate an empty observing slot, with no widget ownership. */
  NodeRef() : slot_(std::make_shared<Slot>()) {
  }

  /** @return Empty before successful mount, otherwise the observing identity, possibly stale. */
  Node<T> node() const noexcept {
    return slot_ ? slot_->node : Node<T>{};
  }

private:
  friend struct detail::NamedAccess;
  friend class Description<T>;
  template <class U> friend class Ref;

  /** @brief Shared identity storage; never owns a widget or its scene. */
  struct Slot {
    Node<T> node;             ///< Published identity, empty until the complete mount succeeds.
    bool pending{};           ///< Reserved by an in-progress mount, including across reentrant calls.
    detail::RefTarget target; ///< Weak scene context for the additive fluent reference.
    bool fluent{};            ///< Only fluent references enforce their immutable construction-thread identity.
  };

  std::shared_ptr<Slot> slot_;
};

/** @brief Typed Widget observation; explicit composite specializations reuse their existing weak facades.
 * @tparam T Public Widget subtype, or a supported composite facade specialization for MessageDialog/ThemeSelector.
 * This primary template provides Widget capabilities; text() is available for Control subtypes and exactly LineEdit.
 * Construct on the receiving UI thread. Publish with Description::reference only after successful mount.
 * Copies share the observation and never own the scene/widget. Copy/destruction may occur on other
 * threads after publication, but operations reject them before reading mutable scene/slot state.
 * Synchronize transfer; publication must not race foreign-thread use. The low-level node() remains UI-only.
 * Construction allocates and can throw; moved-from references produce an empty text observation.
 */
template <class T> class Ref : public NodeRef<T> {
  friend struct detail::MessageAccess;

public:
  /** @brief Allocate an empty observing slot on the UI thread; may throw std::bad_alloc. */
  Ref() {
    this->slot_->fluent = true;
  }

  /** @param value Finite replacement in the inclusive configured range; no clamping.
   * @param origin Public command site retained on failure.
   * @return Checked programmatic change, automatically collected on failure; equal values request no redraw.
   * Requires a live mounted Ref on its UI thread, between frames. Disabled presentation does not block writes.
   */
  std::expected<void, ManagedError> value(double value,
                                          std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, Gauge>
  {
    auto node = this->slot_ ? std::shared_ptr<const Node<T>>(this->slot_, &this->slot_->node) : nullptr;

    return detail::gaugeValue(target(), std::move(node), value, false, origin);
  }

  /** @param delta Finite amount added to the current value; nonfinite/out-of-range results reject without mutation.
   * @param origin Public command site retained on failure.
   * @return Checked programmatic change, automatically collected on failure; unchanged results request no redraw.
   * Uses the same UI-thread, lifetime and between-frame contract as value(double); never clamps.
   */
  std::expected<void, ManagedError> adjust(double delta,
                                           std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, Gauge>
  {
    auto node = this->slot_ ? std::shared_ptr<const Node<T>>(this->slot_, &this->slot_->node) : nullptr;

    return detail::gaugeValue(target(), std::move(node), delta, true, origin);
  }

  /** @param origin Original access call site.
   * @return Owning protected draft identity; plaintext requires an explicit synchronous borrow.
   */
  std::expected<PasswordValue, ManagedError> value(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, Password>
  {
    return detail::passwordValue(target(), origin);
  }

  /** @param value Protected candidate, edited under this field's ordinary publication mode.
   * @param origin Original mutation call site. @return Checked change, automatically collected on failure.
   */
  std::expected<void, ManagedError> value(PasswordValue value,
                                          std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, Password>
  {
    return detail::setPasswordValue(target(), std::move(value), origin);
  }

  /** @tparam F Owned synchronous callable returning void and accepting a scoped SecretView.
   * @param callback Plaintext consumer; views and caller-created copies remain its responsibility.
   * @param origin Original access site. @return Checked secure access, with failures collected once.
   * The borrowed value survives removal of the Ref, widget or scene during the callback.
   */
  template <class F>
    requires(std::same_as<T, Password> && portals::detail::SecretCallback<F> &&
             std::same_as<std::invoke_result_t<std::decay_t<F> &, SecretView>, void>)
  std::expected<void, ManagedError> withPlaintext(F &&callback,
                                                  std::source_location origin = std::source_location::current()) const {
    auto observation = target();
    auto selected = detail::passwordValue(observation, origin);

    if (!selected) {
      return std::unexpected(selected.error());
    }

    auto accessed = portals::detail::PasswordValueAccess::borrowOwned(std::move(*selected), std::forward<F>(callback));

    if (!accessed) {
      auto failure =
        portals::detail::passwordError(ManagedPhase::TEXT, observation->node.id(), std::move(accessed.error()));

      detail::reportPasswordError(observation, failure, origin);

      return std::unexpected(std::move(failure));
    }

    return {};
  }

  /** @param origin Public call site retained on failure.
   * @return Checked start; retains the current frame and arms one fresh interval during ordinary preparation.
   */
  std::expected<void, ManagedError> start(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, Spinner>
  {
    auto node = this->slot_ ? std::shared_ptr<const Node<T>>(this->slot_, &this->slot_->node) : nullptr;
    return detail::spinnerActive(target(), std::move(node), true, origin);
  }

  /** @param origin Public call site retained on failure.
   * @return Checked immediate token cancellation; failures enter the ordinary scene collector.
   */
  std::expected<void, ManagedError> stop(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, Spinner>
  {
    auto node = this->slot_ ? std::shared_ptr<const Node<T>>(this->slot_, &this->slot_->node) : nullptr;
    return detail::spinnerActive(target(), std::move(node), false, origin);
  }

  /** @param value Literal UTF-8 label bytes copied inside the checked allocation boundary.
   * @param origin Public command call site.
   * @return Checked replacement; successful changes reset frame zero and restart its interval.
   */
  std::expected<void, ManagedError> label(std::string_view value,
                                          std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, Spinner>
  {
    auto node = this->slot_ ? std::shared_ptr<const Node<T>>(this->slot_, &this->slot_->node) : nullptr;
    return detail::spinnerLabel(target(), std::move(node), value, origin);
  }

  /** @param value Owning literal/format/resource expression, borrowed only for this synchronous call.
   * @param resolver Explicit synchronous resolver, as with Ref::text; no global catalog is introduced.
   * @param origin Public command call site retained on failure.
   * @return Checked staged replacement with original resource/text failures collected once.
   */
  std::expected<void, ManagedError> label(const TextValue &value, const TextResolver &resolver = {},
                                          std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, Spinner>
  {
    auto node = this->slot_ ? std::shared_ptr<const Node<T>>(this->slot_, &this->slot_->node) : nullptr;
    return detail::spinnerLabel(target(), std::move(node), value, resolver, origin);
  }

  /** @param origin Operation site. @return Pure checked scope entry; already open is a no-op. */
  std::expected<void, ManagedError> open(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, HelpView>
  {
    return detail::helpCommand(target(), true, origin);
  }

  /** @param origin Operation site. @return Pure checked focus return; already closed is a no-op. */
  std::expected<void, ManagedError> close(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, HelpView>
  {
    return detail::helpCommand(target(), false, origin);
  }

  /** @param axis Selected logical axis.
   * @param offset Exact logical offset, clamped to the current range.
   * @param origin Command site retained by the scene collector.
   * @return Checked programmatic command or reported affinity/liveness/lease error.
   */
  std::expected<void, ManagedError> setOffset(ScrollAxis axis, ScrollOffset offset,
                                              std::source_location origin = std::source_location::current()) const
    requires(std::derived_from<T, ScrollArea> || std::same_as<T, ScrollBar>)
  {
    return detail::scrollOffset(target(), axis, offset, origin);
  }

  /** @param bytes Complete UTF-8 batch, borrowed only during synchronous admission.
   * @param origin Original command site. @return Atomic owning receipt; checked failures are never auto-reported.
   */
  std::expected<LogAppendReceipt, ManagedError> appendBatch(
    std::string_view bytes, std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LogView>
  {
    const auto observation = target();
    const auto owner = detail::eventScene(observation);

    if (!owner) {
      return std::unexpected(owner.error());
    }

    auto result = (*owner)->appendLogBatch(this->node(), bytes);

    if (!result) {
      result.error().registrationOrigin = origin;
    }

    return result;
  }

  /** @param bytes Complete batch. @param origin Call site.
   * @return Success; false reports the exact native failure once through the existing scene collector.
   */
  bool append(std::string_view bytes, std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LogView>
  {
    const auto result = appendBatch(bytes, origin);

    if (!result) {
      detail::reportEventError(target(), result.error(), origin);
    }

    return result.has_value();
  }

  /** @param origin Original call site. @return Pure checked resume-following result, without reporting. */
  std::expected<void, ManagedError> followTail(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LogView>
  {
    const auto owner = detail::eventScene(target());

    if (!owner) {
      return std::unexpected(owner.error());
    }

    auto result = (*owner)->followLogTail(this->node());

    if (!result) {
      result.error().registrationOrigin = origin;
    }

    return result;
  }

  /** @param origin Call site. @return Resume success; false records the original cause once. */
  bool tail(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LogView>
  {
    const auto result = followTail(origin);

    if (!result) {
      detail::reportEventError(target(), result.error(), origin);
    }

    return result.has_value();
  }

  /** @param origin Original call site. @return Checked clear result preserving IDs; never auto-reports. */
  std::expected<void, ManagedError> clearLog(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LogView>
  {
    const auto owner = detail::eventScene(target());

    if (!owner) {
      return std::unexpected(owner.error());
    }

    auto result = (*owner)->clearLog(this->node());

    if (!result) {
      result.error().registrationOrigin = origin;
    }

    return result;
  }

  /** @param origin Call site. @return Clear success; false records the original failure once. */
  bool clear(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LogView>
  {
    const auto result = clearLog(origin);

    if (!result) {
      detail::reportEventError(target(), result.error(), origin);
    }

    return result.has_value();
  }

  /** @return Owning counters/flags or exact UI/lifetime/frame error; never reports or retains records. */
  std::expected<LogStatus, ManagedError> status() const
    requires std::same_as<T, LogView>
  {
    const auto owner = detail::eventScene(target());

    return owner ? (*owner)->logStatus(this->node()) : std::unexpected(owner.error());
  }

  /** @param metrics Exact standalone extent and viewport.
   * @param origin Command site retained by the scene collector.
   * @return Checked metrics command; an area projection is not a metrics writer.
   */
  std::expected<void, ManagedError> setMetrics(ScrollAxisMetrics metrics,
                                               std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, ScrollBar>
  {
    const auto observation = target();
    auto owner = detail::eventScene(observation);
    if (!owner) {
      return std::unexpected(owner.error());
    }
    auto result = (*owner)->setScrollMetrics(this->node(), metrics);
    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }
    return result;
  }

  /** @param origin Request site retained by the scene collector.
   * @return Checked one-shot reveal admission; final rendering remains asynchronous.
   */
  std::expected<void, ManagedError> ensureVisible(std::source_location origin = std::source_location::current()) const {
    return detail::ensureVisible(target(), origin);
  }

  /** @return Owning final local/surface/clip geometry, or checked affinity/liveness/reentry failure.
   * Before final layout, while hidden, or after invalidation/rearrangement returns INVALID_GEOMETRY.
   * Does not perform layout or report errors; copies never borrow widgets or the scene.
   */
  std::expected<NodeGeometry, ManagedError> geometry() const {
    return detail::geometry(target());
  }

  /** @return Owning final local size or the exact geometry() error; never performs layout. */
  std::expected<CellSize, ManagedError> size() const {
    return geometry().transform([](const NodeGeometry &value) {
      return value.local.size();
    });
  }

  /** @tparam Callback Owning compact action receiving const NodeGeometry& or no arguments.
   * @param callback Void, checked TreeError or EditorCommandResult action retained by the scene.
   * @param origin Registration site retained in diagnostics.
   * @return Scene-owned registration or checked admission/budget/allocation error.
   * Initial valid and changed local/surface/clip bounds arrive on a subsequent UI dispatch.
   * Removal/close retires captures; an executing capture survives self-removal.
   */
  template <class Callback>
    requires portals::detail::ManagedAction<Callback, NodeGeometry>
  std::expected<void, ManagedError> onLayout(Callback callback,
                                             std::source_location origin = std::source_location::current()) const {
    auto observation = target();
    auto owner = detail::eventScene(observation);
    if (!owner) {
      return std::unexpected(owner.error());
    }
    auto result = [&]() -> std::expected<void, ManagedError> {
      try {
        return (*owner)->onLayout(this->node(), std::move(callback), origin);
      } catch (const std::bad_alloc &) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, this->node().id(), TreeError::NO_MEMORY, {}, {}});
      } catch (...) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, this->node().id(), TreeError::CALLBACK_FAILED, {}, {}});
      }
    }();
    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }
    return result;
  }

  /** @tparam Callback Owning compact click action, optionally taking const ClickEvent&.
   * @param action Void, expected<Value, TreeError> or exact EditorCommandResult callback; success values are ignored.
   * @param origin Registration call site retained in diagnostics.
   * @return Checked registration on a mounted live button; CLOSED before publication/destruction,
   * WRONG_THREAD, STALE_NODE, duplicate INVALID_ARGUMENT or allocation error. Scene owns captures/token.
   * This adds one binding; it never replaces an existing one. Admitted calls survive self-removal.
   * Registration failures are also recorded in a live UI collector, without stopping the scene.
   * Internal callback transfers are checked; constructing the by-value argument before entry may throw.
   */
  template <class Callback = detail::QuitCommand>
    requires(std::same_as<T, Button> && portals::detail::ManagedAction<Callback, ClickEvent>)
  std::expected<void, ManagedError> onClick(Callback action,
                                            std::source_location origin = std::source_location::current()) const {
    detail::commandOrigin(action, origin);
    auto observation = target();
    auto scene = detail::eventScene(observation);

    if (!scene) {
      return std::unexpected(scene.error());
    }

    // The forwarding move into the checked scene API is still part of this boundary.
    auto result = [&]() -> std::expected<void, ManagedError> {
      try {
        return (*scene)->onClick(this->node(), std::move(action));
      } catch (const std::bad_alloc &) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, this->node().id(), TreeError::NO_MEMORY, {}, {}});
      } catch (...) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, this->node().id(), TreeError::CALLBACK_FAILED, {}, {}});
      }
    }();

    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }

    return result;
  }

  /** @param value Owning explicit presentation for an own-value composite; older generations are ignored.
   * @param origin Public call site for the existing short diagnostic collector.
   * @return Original checked TreeError; bound/automatic composites reject writes with INVALID_ARGUMENT.
   */
  std::expected<void, TreeError> display(DataPresentation value,
                                         std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, DataStateView>
  {
    return detail::displayDataState(target(), value, origin);
  }

  /** @param origin Focus request call site retained in managed diagnostics.
   * @return Checked focus/entry result; CLOSED before publication/after scene destruction,
   * WRONG_THREAD, STALE_NODE, typed eligibility or callback failure. No lifetime is extended.
   * Programmatic focus preserves the same editor blur and validation policies as keyboard focus.
   */
  std::expected<void, ManagedError> focus(std::source_location origin = std::source_location::current()) const {
    return focusOperation(&ManagedScene::focus, origin);
  }

  /** @param origin Activation call site retained by the existing callback error collector.
   * @return Checked activation; ignored failures in an admitted short callback remain reported once.
   */
  DialogCommandResult open(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, Dialog>
  {
    return detail::dialogCommand(target(), true, origin);
  }

  /** @param origin Dismissal call site retained by the existing callback error collector.
   * @return Checked dismissal; a closed layer is a no-op and admitted callbacks never replay.
   */
  DialogCommandResult dismiss(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, Dialog>
  {
    return detail::dialogCommand(target(), false, origin);
  }

  /** @return Effective activation, or exact affinity/lifetime/identity failure without reporting. */
  std::expected<bool, ManagedError> opened() const
    requires std::same_as<T, Dialog>
  {
    auto observation = target();
    auto owner = detail::eventScene(observation);
    return owner ? (*owner)->dialogOpened(observation->node) : std::unexpected(owner.error());
  }

  /** @param origin Focus-entry call site retained in managed diagnostics.
   * @return Checked focus-only scope entry, saving a return record; no input trap is installed.
   * Published callback errors retain entry and can be followed by leaveFocusScope().
   */
  std::expected<void, ManagedError>
  enterFocusScope(std::source_location origin = std::source_location::current()) const {
    return focusOperation(&ManagedScene::enterFocusScope, origin);
  }

  /** @param origin Focus-return call site retained in managed diagnostics.
   * @return Checked top-scope return; stale/unpublished/foreign-thread errors follow focus().
   * Owned failures are reported once, allowing use in ordinary void button callbacks.
   */
  std::expected<void, ManagedError>
  leaveFocusScope(std::source_location origin = std::source_location::current()) const {
    return focusOperation(&ManagedScene::leaveFocusScope, origin);
  }

  /** @param resolver Optional owning synchronous adapter used only for i18n expressions.
   * @param origin Property acquisition call site used by assignment/+= diagnostics.
   * @return Owning observing proxy, valid after Ref destruction; never extends scene/widget lifetime.
   * Resolver capture/storage allocation may throw before a mutation starts.
   */
  TextProperty text(TextResolver resolver = {}, std::source_location origin = std::source_location::current()) const
    requires(std::derived_from<T, Control> || std::same_as<T, LineEdit>)
  {
    auto target = this->slot_ ? std::shared_ptr<detail::RefTarget>(this->slot_, &this->slot_->target) : nullptr;
    constexpr std::string_view TYPE = std::same_as<T, LineEdit>     ? "LineEdit"
                                      : std::same_as<T, Label>      ? "Label"
                                      : std::same_as<T, Button>     ? "Button"
                                      : std::same_as<T, CheckBox>   ? "CheckBox"
                                      : std::same_as<T, ThreeState> ? "ThreeState"
                                                                    : "Control";

    return TextProperty(std::move(target), std::move(resolver), TYPE, origin);
  }

  /** @return Query editor observation; acquire after successful mount. */
  Ref<LineEdit> editor() const
    requires std::same_as<T, FindBar>
  {
    return detail::findEditor(target());
  }

  /** @param resolver Optional owning text resolver. @param origin Call site retained for mutations.
   * @return Lazy
   * ordinary query draft proxy, including when acquired before mount.
   */
  TextProperty text(TextResolver resolver = {}, std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, FindBar>
  {
    return TextProperty(target(), std::move(resolver), "FindBar", origin);
  }

  /** @param direction Checked stored-prefix direction. @return Selected owning range or exact technical failure.
   *
   * Empty/no-match is an empty successful handle. Same-index navigation makes no callback or invalidation.
   * A
   * selected callback runs after publication; its failure does not roll back the selected range.
   */
  std::expected<FindMatch, ManagedError> navigate(FindDirection direction) const
    requires std::same_as<T, FindBar>
  {
    return detail::findNavigate(target(), direction);
  }

  /** @param origin Original call site. @return True when a stored match exists; technical errors report once. */
  bool next(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, FindBar>
  {
    auto observation = target();
    auto result = detail::findNavigate(observation, FindDirection::NEXT);
    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }
    return result && static_cast<bool>(*result);
  }

  /** @param origin Original call site. @return True when a stored match exists; technical errors report once. */
  bool previous(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, FindBar>
  {
    auto observation = target();
    auto result = detail::findNavigate(observation, FindDirection::PREVIOUS);
    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }
    return result && static_cast<bool>(*result);
  }

  /** @return Current owning range or empty handle; never reports, scans or changes selection. */
  std::expected<FindMatch, ManagedError> match() const
    requires std::same_as<T, FindBar>
  {
    return detail::findMatch(target());
  }

  /** @param value Previously retained match. @return True for membership in the current result set,
   * independently
   * of the selected ordinal; false for empty/stale sets, or an affinity/liveness error.
   */
  std::expected<bool, ManagedError> current(const FindMatch &value) const
    requires std::same_as<T, FindBar>
  {
    return detail::findCurrent(target(), value);
  }

  /** @param bytes New bounded UTF-8 source, borrowed only during this call.
   * @return Checked replacement; creates
   * new identity even for equal bytes, including while disabled/hidden.
   * Rejected input/preparation allocation
   * preserves the previous source/results/selection and original TextError. The selected callback runs after
   * publication; its failure does not roll back the accepted source.
   */
  std::expected<void, ManagedError> replaceDocument(std::string_view bytes) const
    requires std::same_as<T, FindBar>
  {
    return detail::findDocument(target(), bytes);
  }

  /** @param bytes New owning source candidate. @param origin Original call site retained in diagnostics.
   * @return
   * True after coherent replacement; false after reporting the original technical failure once.
   */
  bool document(std::string_view bytes, std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, FindBar>
  {
    auto observation = target();
    auto result = detail::findDocument(observation, bytes);
    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }
    return result.has_value();
  }

  /** @param origin Original call site for the ordinary editor diagnostic receipt.
   * @return Explicit clear receipt; one editor history operation, confirmed in every mode.
   * Technical failures are reported once automatically. Keep the opaque receipt to preserve
   * diagnostic provenance; result() extracts only its checked TreeError observation.
   */
  EditorCommandResult clearQuery(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, FindBar>
  {
    return detail::findClear(target(), origin);
  }

  /** @param origin Original call site. @return True after clear/no-op; technical failure is reported once. */
  bool clear(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, FindBar>
  {
    return static_cast<bool>(detail::findClear(target(), origin));
  }

  /** @return Standard text editor observation; failure remains in its checked slot.
   * Obtain only after mount;
   * custom non-LineEdit roots reject with INVALID_ARGUMENT.
   */
  Ref<LineEdit> editor() const
    requires std::same_as<T, FormField>
  {
    return detail::fieldEditor(target());
  }

  /** @param resolver Optional owning localization adapter. @param origin Text mutation call site.
   * @return Existing
   * text proxy forwarding to this field's standard editor; no model ownership is added.
   */
  TextProperty text(TextResolver resolver = {}, std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, FormField>
  {
    return TextProperty(target(), std::move(resolver), "FormField", origin);
  }

  /** @return Most recent validation presentation, or exact checked affinity/liveness error.
   * Deferred-mode dirty
   * drafts preserve the last message until their next validation boundary.
   */
  std::expected<FieldValidation, ManagedError> validation() const
    requires std::same_as<T, FormField>
  {
    return detail::fieldValidation(target());
  }

  /** @return True when the current standard editor draft passes the synchronous field rules.
   * This explicit check
   * updates presentation without committing a model or adding an Undo operation.
   * Reentry returns WOULD_BLOCK;
   * custom editor roots return INVALID_ARGUMENT.
   */
  std::expected<bool, ManagedError> validate() const
    requires std::same_as<T, FormField>
  {
    return detail::validateField(target());
  }

  /** @return True after the current draft passes its synchronous rules.
   * @param origin Validation call site
   * retained by the existing diagnostic collector.
   * Short callback convenience: runs rules and updates
   * presentation; technical failures use the scene error channel.
   * Use validate() to inspect the original checked
   * failure explicitly.
   */
  bool valid(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, FormField>
  {
    auto observation = target();
    auto result = detail::validateField(observation);

    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }

    return result && *result;
  }

  /** @param value Owning explicit presentation, without asynchronous completion admission.
   * @return Exact checked
   * update. Pending never disables the editor or schedules work.
   */
  std::expected<void, ManagedError> display(FieldValidation value) const
    requires std::same_as<T, FormField>
  {
    return detail::displayField(target(), std::move(value));
  }

  /** @param change Owning policy override for every mutation of this proxy; does not change editor options.
   * @param resolver Optional owning synchronous resource adapter.
   * @param origin Property acquisition call site retained in diagnostics.
   * @return Observing editor proxy; RESET_BASELINE also clears equal-text history without redraw.
   */
  TextProperty text(ProgrammaticChange change, TextResolver resolver = {},
                    std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return TextProperty(target(), std::move(resolver), "LineEdit", origin, change);
  }

  /** @param origin Command call site retained in diagnostics.
   * @return Owning checked Undo result, with exact reporting provenance for managed button adapters.
   */
  EditorCommandResult undo(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorCommand(target(), false, false, origin);
  }

  /** @param origin Command call site retained in diagnostics.
   * @return Owning checked Redo result; empty history is a no-op without callbacks or redraw.
   */
  EditorCommandResult redo(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorCommand(target(), true, false, origin);
  }

  /** @brief Clear the visible draft as one user operation, applying the configured EditMode.
   * @param origin Command call site retained in diagnostics.
   * @return Checked technical result with reporting provenance; semantic validation/conflict uses LineEditResult.
   * Empty writable drafts are no-ops. Read-only/disabled editors return WOULD_BLOCK. Return the complete
   * EditorCommandResult from managed callbacks to preserve report-once behavior.
   */
  EditorCommandResult clear(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorCommand(target(), false, false, origin, true);
  }

  /** @param origin Command call site retained in diagnostics.
   * @return Owning checked grouping boundary; does not change text or presentation.
   */
  EditorCommandResult stopGrouping(std::source_location origin = std::source_location::current()) const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorCommand(target(), false, true, origin);
  }

  /** @return Read-only visible caption source; acquisition never reads or subscribes. */
  TextSource captionText() const
    requires std::derived_from<T, Control>
  {
    return TextSource(target(), TextSource::Kind::CAPTION);
  }

  /** @return Deferred physical body-row source; acquisition never reads or subscribes.
   * Retains the original observing slot, including when acquired before mount, without owning the scene.
   */
  auto visibleModel() const
    requires requires { typename detail::TableRowType<T>::Type; }
  {
    using Row = typename detail::TableRowType<T>::Type;
    auto node = this->slot_ ? std::shared_ptr<const Node<T>>(this->slot_, &this->slot_->node) : nullptr;

    return VisibleModelSource<Row>(target(), std::move(node));
  }

  /** @return Read-only visible draft source, including uncommitted or semantically rejected text. */
  TextSource draftText() const
    requires std::same_as<T, LineEdit>
  {
    return TextSource(target(), TextSource::Kind::DRAFT);
  }

  /** @return Read-only latest accepted model source, independent of the editor's visible draft. */
  TextSource committedText() const
    requires std::same_as<T, LineEdit>
  {
    return TextSource(target(), TextSource::Kind::COMMITTED);
  }

  /** @return Effective checked availability without adoption, invalidation or error reporting. */
  std::expected<HistoryAvailability, TreeError> historyAvailability() const
    requires std::same_as<T, LineEdit>
  {
    return detail::editorAvailability(target());
  }

  /** @return Effective Undo availability; false on checked errors without reporting. */
  bool canUndo() const
    requires std::same_as<T, LineEdit>
  {
    const auto value = historyAvailability();

    return value && value->canUndo;
  }

  /** @return Effective Redo availability; false on checked errors without reporting. */
  bool canRedo() const
    requires std::same_as<T, LineEdit>
  {
    const auto value = historyAvailability();

    return value && value->canRedo;
  }

private:
  friend struct detail::ClickAccess;
  friend struct detail::EditorEventAccess;
  friend struct detail::ChangeAccess;
  friend class KeyInfo;
  friend struct detail::KeyHelpAccess;
  friend struct detail::LayoutAccess;
  friend struct detail::NamedAccess;
  friend class DynamicRef;
  template <class U> friend class Description;

  std::expected<void, ManagedError>
  focusOperation(std::expected<void, ManagedError> (ManagedScene::*operation)(Node<Widget>),
                 std::source_location origin) const {
    auto observation = target();
    auto owner = detail::eventScene(observation);

    if (!owner) {
      return std::unexpected(owner.error());
    }

    auto result = ((*owner)->*operation)(this->node());

    if (!result) {
      detail::reportFocusError(observation, result.error(), origin);
    }

    return result;
  }

  std::shared_ptr<detail::RefTarget> target() const {
    return this->slot_ ? std::shared_ptr<detail::RefTarget>(this->slot_, &this->slot_->target) : nullptr;
  }
};

namespace detail {
/** @brief Internal checked named publication bridge; no widget or scene ownership escapes. */
struct NamedAccess {
  /** @param stage Existing owning style payload.
   * @param target Weak exact-node observation.
   * @return Independent scoped stage using the same existing style implementation.
   */
  static StyleStage scopeStyle(const StyleStage &stage, std::shared_ptr<RefTarget> target);

  /** @param reference Dynamic observation.
   * @return Shared weak target with no scene ownership.
   */
  static std::shared_ptr<RefTarget> target(const DynamicRef &reference);

  /** @param scene Receiving managed owner.
   * @param target Shared observation, checked before reading mutable fields.
   * @return Exact node scope or typed thread/owner/liveness failure.
   */
  static std::expected<NodeId, ManagedError> scope(ManagedScene &scene, const std::shared_ptr<RefTarget> &target) {
    auto owner = eventScene(target);

    if (!owner) {
      auto failure = owner.error();
      failure.phase = ManagedPhase::STYLE;

      return std::unexpected(failure);
    }

    if (*owner != &scene) {
      return std::unexpected(ManagedError{ManagedPhase::STYLE, target->node.id(), TreeError::WRONG_SESSION, {}, {}});
    }

    auto live = scene.boundary(ManagedPhase::STYLE, target->node);

    return live ? std::expected<NodeId, ManagedError>(target->node.id()) : std::unexpected(live.error());
  }

  /** @param scene Receiving managed owner.
   * @param source Original dynamic observation, never resolved again by name.
   * @return Original live identity, or exact prior lookup/type/liveness/owner error.
   */
  static std::expected<Node<Widget>, ManagedError> observe(ManagedScene &scene,
                                                           const std::shared_ptr<RefTarget> &source) {
    auto owner = eventScene(source);

    if (!owner) {
      return std::unexpected(owner.error());
    }

    if (*owner != &scene) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, source->node.id(), TreeError::WRONG_SESSION, {}, {}});
    }

    auto allowed = scene.boundary(ManagedPhase::BIND, source->node);

    return allowed ? std::expected<Node<Widget>, ManagedError>(source->node) : std::unexpected(allowed.error());
  }

  /** @param scene Borrowed live managed owner.
   * @param name Case-sensitive ID; empty, absent and duplicate names reject.
   * @return Unique live managed identity, or exact boundary/name error.
   */
  static std::expected<Node<Widget>, ManagedError> find(ManagedScene &scene, std::string_view name) {
    auto allowed = scene.boundary(ManagedPhase::BIND, scene.root_);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    Node<Widget> found;

    for (const auto &entry : scene.entries_) {
      if (entry->removed || !entry->target || entry->target->id != name || name.empty()) {
        continue;
      }

      auto live = scene.app_.tree().read(entry->node, [](const Widget &) {
      });

      if (!live && (live.error() == TreeError::STALE_NODE || live.error() == TreeError::CLOSED)) {
        continue;
      }

      if (!live) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, entry->node.id(), live.error(), {}, {}});
      }

      if (found != Node<Widget>{}) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, entry->node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
      }

      found = entry->node;
    }

    if (found == Node<Widget>{}) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::INVALID_ARGUMENT, {}, {}});
    }

    return found;
  }

  /** @param scene Existing managed owner. @param node Field identity to inspect.
   * @return Exact standard editor identity, empty for a custom root, or the original tree error.
   */
  static std::expected<Node<LineEdit>, ManagedError> fieldDraft(ManagedScene &scene, Node<Widget> node) {
    Node<Widget> editor;
    auto checked = scene.app_.tree().read(node, [&](const Widget &widget) {
      if (const auto *field = dynamic_cast<const FormField *>(&widget)) {
        editor = field->editor();
      } else if (const auto *find = dynamic_cast<const FindBar *>(&widget)) {
        editor = find->editor();
      }
    });

    if (!checked) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), checked.error(), {}, {}});
    }

    Node<LineEdit> result;

    checked = scene.app_.tree().read(editor, [&](const Widget &widget) {
      if (dynamic_cast<const LineEdit *>(&widget)) {
        result = Node<LineEdit>(editor.id());
      }
    });

    return checked ? std::expected<Node<LineEdit>, ManagedError>(result)
                   : std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), checked.error(), {}, {}});
  }

  /** @tparam T Public Widget subtype checked with RTTI before constructing its observer.
   * @param reference Empty shared observing slot to publish.
   * @param scene Existing owner, borrowed only during checked validation.
   * @param node Unique name result; no unchecked downcast is exposed.
   * @return Success after one publication or INVALID_ARGUMENT for a wrong type.
   */
  template <class T>
  static std::expected<void, ManagedError> publish(const Ref<T> &reference, ManagedScene &scene, Node<Widget> node) {
    auto &slot = *reference.slot_;
    bool compatible{};
    Node<Control> caption;
    Node<LineEdit> draft;
    Node<Password> secret;
    auto checked = scene.app_.tree().read(node, [&](const Widget &widget) {
      if constexpr (std::same_as<T, Widget>) {
        compatible = true;
      } else {
        compatible = dynamic_cast<const T *>(&widget) != nullptr;
      }

      if (dynamic_cast<const Control *>(&widget)) {
        caption = Node<Control>(node.id());
      }

      if (dynamic_cast<const LineEdit *>(&widget)) {
        draft = Node<LineEdit>(node.id());
      }
      if (dynamic_cast<const Password *>(&widget)) {
        secret = Node<Password>(node.id());
      }
    });

    if (!checked || !compatible) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, node.id(), checked ? TreeError::INVALID_ARGUMENT : checked.error(), {}, {}});
    }

    if constexpr (std::same_as<T, FormField> || std::same_as<T, FindBar>) {
      auto editor = fieldDraft(scene, node);

      if (!editor) {
        return std::unexpected(editor.error());
      }

      draft = *editor;
    }

    slot.node = Node<T>(node.id());
    slot.target.scene = &scene;
    slot.target.lifetime = scene.lifetime_;
    slot.target.node = node;
    slot.target.caption = caption;
    slot.target.draft = draft;
    slot.target.secret = secret;

    return {};
  }

  /** @tparam T Observed public widget type.
   * @param reference Shared slot retaining this request's original failure.
   * @param failure Exact failure copied without widget access.
   */
  template <class T> static void reject(const Ref<T> &reference, const ManagedError &failure) {
    reference.slot_->target.failure = std::make_shared<const ManagedError>(failure);
  }

  /** @param scene Existing collector.
   * @param failure Exact cause stamped with short-error provenance.
   * @param source Owned name and static expected-type diagnostic.
   */
  static void report(ManagedScene &scene, ManagedError &failure, const LookupSource &source) {
    scene.reportShortError(failure, "scene.get", source.name, source.expectedType, source.origin);
  }

  /** @tparam T Observed widget subtype.
   * @param reference Existing typed observation.
   * @return Shared weak target, never scene or widget ownership.
   */
  template <class T> static std::shared_ptr<RefTarget> target(const Ref<T> &reference) {
    return reference.target();
  }

  /** @tparam T Public Widget subtype required by this request.
   * @param request Name and weak facade identity, copied through fallible owning transfers.
   * @return Typed slot with deferred publication or its retained admission failure.
   */
  template <class T> static Ref<T> request(const LookupSource &request) {
    Ref<T> reference;
    LookupSource source(request);

    source.expectedType = typeid(T).name();

    auto result =
      requestLookup(source, [reference](ManagedScene &scene, std::expected<Node<Widget>, ManagedError> node) {
        auto published =
          node ? publish(reference, scene, *node) : std::expected<void, ManagedError>(std::unexpected(node.error()));

        if (!published) {
          reject(reference, published.error());
        }

        return published;
      });

    if (!result) {
      reject(reference, result.error());
    }

    return reference;
  }
};
} // namespace detail

/** @brief Dynamic named observation with checked caption/editor and Button capabilities.
 * Copies own the request name and observing slot; they never retain a scene/widget.
 * Converting to Ref<T> checks the same published NodeId, or waits for this slot before mount.
 */
class DynamicRef {
public:
  /** @tparam T Public Widget subtype tested during deferred/immediate name resolution.
   * @return Typed weak slot; wrong type records INVALID_ARGUMENT before startup or in the live collector.
   * Conversion construction may allocate. A successful slot never retargets on later ID reuse.
   */
  template <class T>
    requires std::derived_from<T, Widget>
  operator Ref<T>() const {
    auto source = source_;

    source.observation = detail::NamedAccess::target(reference_);

    return detail::NamedAccess::request<T>(source);
  }

  /** @param resolver Optional owning synchronous localization adapter.
   * @param origin Property acquisition call site retained for short mutations.
   * @return Existing checked text proxy; non-text widgets return INVALID_ARGUMENT.
   */
  TextProperty text(TextResolver resolver = {}, std::source_location origin = std::source_location::current()) const {
    return TextProperty(reference_.target(), std::move(resolver), "Widget", origin);
  }

  /** @tparam Callback Owning compact Button action accepted by Ref<Button>::onClick.
   * @param callback Action transferred into the existing managed binding.
   * @param origin Registration call site.
   * @return Checked Button registration result; use a pipe stage for registration before preparation.
   */
  template <class Callback = detail::QuitCommand>
    requires portals::detail::ManagedAction<Callback, ClickEvent>
  std::expected<void, ManagedError> onClick(Callback callback,
                                            std::source_location origin = std::source_location::current()) const {
    Ref<Button> typed = *this;

    return typed.onClick(std::move(callback), origin);
  }

  /** @return Observing published identity, empty before mount; UI-thread only. */
  Node<Widget> node() const noexcept {
    return reference_.node();
  }

private:
  friend class Scene;
  friend struct detail::NamedAccess;

  explicit DynamicRef(const detail::LookupSource &source)
      : source_(source), reference_(detail::NamedAccess::request<Widget>(source_)) {
  }

  detail::LookupSource source_;
  Ref<Widget> reference_;
};

inline std::shared_ptr<detail::RefTarget> detail::NamedAccess::target(const DynamicRef &reference) {
  return reference.reference_.target();
}

} // namespace portals::ui
