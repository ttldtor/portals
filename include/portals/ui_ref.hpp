// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/managed.hpp>
#include <portals/text_value.hpp>

namespace portals::ui {
namespace detail {
/** @brief Shared observation; publication and all mutable fields belong to the constructing UI thread. */
struct RefTarget {
  const std::thread::id owner = std::this_thread::get_id(); ///< Immutable affinity checked before mutable fields.
  ManagedScene *scene{};                                    ///< Borrowed only while lifetime remains live on UI.
  std::weak_ptr<const void> lifetime;                       ///< Does not retain the scene or its widgets.
  Node<Widget> node;                                        ///< Published identity, never automatically reset.
  Node<Control> caption;                                    ///< Typed caption identity, empty for other widgets.
  Node<LineEdit> draft;                                     ///< Typed editor identity, empty for other widgets.
};

struct RefAccess;
/** @brief Checked bridge preserving immutable Ref affinity before reading its mutable published target.
 * @param target Owning observation with weak scene lifetime; may be empty.
 * @param redo Select Redo instead of Undo when stop is false.
 * @param stop Select a grouping boundary instead of a history restoration.
 * @param origin Public command call site retained in diagnostics.
 * @return Owning checked result with exactly its own reporting receipt.
 */
EditorCommandResult editorCommand(std::shared_ptr<RefTarget> target, bool redo, bool stop, std::source_location origin);
/** @param target Owning observing slot, without scene/widget ownership.
 * @return Checked effective availability without reporting, adoption or invalidation.
 */

std::expected<HistoryAvailability, TreeError> editorAvailability(std::shared_ptr<RefTarget> target);

/** @param target Observing slot checked for UI affinity and live scene ownership.
 * @return Live managed scene or typed BIND error; never reads mutable target fields from a foreign thread.
 */
std::expected<ManagedScene *, ManagedError> eventScene(const std::shared_ptr<RefTarget> &target);

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

} // namespace detail

/** @brief Explicit owning synchronous localization adapter; no global locale or resource storage. */
using TextResolver = std::function<std::expected<std::string, TextResourceError>(std::string_view)>;

class TextProperty;

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
