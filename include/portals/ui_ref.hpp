// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/dialog_command_result.hpp>
#include <portals/managed.hpp>
#include <portals/text_value.hpp>

namespace portals::ui {
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
};

struct RefAccess;
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
    return TextSource(target_, expectedType_ == "LineEdit" ? TextSource::Kind::DRAFT : TextSource::Kind::CAPTION);
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
} // namespace portals::ui
