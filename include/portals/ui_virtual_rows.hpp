// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cmath>
#include <portals/background.hpp>
#include <portals/model_position_finite.hpp>
#include <portals/ui_controls.hpp>
#include <portals/ui_model_edit.hpp>
#include <portals/ui_mount.hpp>
#include <portals/virtual_rows.hpp>

namespace portals::ui::detail {
/// @cond INTERNAL
struct RowAccess {
  struct Scope {
    ManagedScene &scene;
    std::shared_ptr<portals::detail::ManagedOrigin> previous;
    std::weak_ptr<const void> lifetime;
    RowDeclarationOriginScope declarations;

    Scope(ManagedScene &owner, std::shared_ptr<portals::detail::ManagedOrigin> origin)
        : scene(owner), previous(std::exchange(owner.rowOrigin_, std::move(origin))), lifetime(Access::lifetime(owner)),
          declarations(owner.rowOrigin_) {
    }

    ~Scope() {
      if (!lifetime.expired()) {
        scene.rowOrigin_ = std::move(previous);
      }
    }
  };

  static std::shared_ptr<const void> layoutLease(ManagedScene &scene) {
    auto lease = scene.rowLayoutLease_.lock();
    if (!lease) {
      lease = std::make_shared<const char>(0);
      scene.rowLayoutLease_ = lease;
    }
    return lease;
  }

  static WidgetTree &tree(ManagedScene &scene) {
    return scene.app_.tree();
  }

  static std::expected<void, ManagedError> background(ManagedScene &scene) {
    auto allowed = scene.boundary(ManagedPhase::BIND, scene.root_);
    if (allowed) {
      scene.asyncEnabled_ = true;
    }
    return allowed;
  }

  static Application &app(ManagedScene &scene) {
    return scene.app_;
  }

  static std::expected<void, ManagedError> admit(ManagedScene &scene, std::shared_ptr<SceneActivity> activity) {
    return scene.admitActivity(std::move(activity));
  }

  static std::expected<void, ManagedError> syncCaption(ManagedScene &scene, Node<Label> node) {
    auto entry = scene.find(node);
    if (!entry || !entry->caption) {
      return std::unexpected(error(node, TreeError::CLOSED));
    }
    return scene.syncCaption(node, *entry->caption, ManagedPhase::PREPARE);
  }

  static std::expected<void, ManagedError> bindCaption(ManagedScene &scene, Node<Label> node,
                                                       std::shared_ptr<PreparedCaptionSource> source) {
    auto allowed = scene.boundary(ManagedPhase::BIND, node);
    if (!allowed) {
      return allowed;
    }
    auto entry = scene.find(node);
    if (!entry || entry->caption) {
      return std::unexpected(error(node, TreeError::INVALID_ARGUMENT));
    }
    auto candidate = std::make_unique<ManagedScene::CaptionBinding>();
    candidate->node = Node<Control>(node);
    candidate->source = std::make_shared<ValueCaptionBinding>();
    candidate->source->type = &PREPARED_CAPTION_TYPE;
    candidate->source->source = std::move(source);
    auto synced = scene.syncCaption(node, *candidate, ManagedPhase::BIND);
    if (!synced) {
      return synced;
    }
    entry = scene.find(node);
    if (!entry) {
      return std::unexpected(error(node, TreeError::CLOSED));
    }
    entry->caption.swap(candidate);
    return {};
  }

  static std::expected<void, TreeError>
  validateEditors(ManagedScene &scene, Node<Widget> root,
                  const std::vector<std::weak_ptr<portals::detail::EditTextBinding>> &bindings, bool custom) {
    for (const auto &entry : scene.entries_) {
      auto within = scene.app_.tree().contains(root, entry->node);
      if (!within) {
        return std::unexpected(within.error());
      }
      if (!*within) {
        continue;
      }
      bool editor{};
      auto read = scene.app_.tree().read(entry->node, [&](const Widget &widget) {
        editor = dynamic_cast<const LineEdit *>(&widget) != nullptr;
      });
      if (!read) {
        return std::unexpected(read.error());
      }
      if (!editor) {
        continue;
      }
      const auto known = std::ranges::any_of(bindings, [&](const auto &weak) {
        auto binding = weak.lock();
        return binding && !binding->closed && binding->receiver == entry->node;
      });
      if (!known && !custom) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }
    return {};
  }

  template <class T> static auto source(ManagedModel<T> model) {
    return model.lock();
  }

  template <class Row>
  static std::expected<void, ManagedError> boolean(ManagedScene &scene, Node<CheckBox> node,
                                                   EditField<Row, bool> field) {
    auto entry = scene.bindingEntry(node);
    if (!entry) {
      return std::unexpected(entry.error());
    }
    auto endpoint = std::make_unique<portals::detail::CheckBoxEndpoint>();
    endpoint->read = [field]() -> std::expected<std::shared_ptr<const PropertyChange<bool>>, TreeError> {
      auto value = field.snapshot();
      if (!value) {
        return std::unexpected(value.error());
      }
      const bool showDraft = value->status == EditStatus::DIRTY || value->status == EditStatus::PENDING;
      auto shown = showDraft && value->draft ? value->draft : value->accepted;
      if (!shown) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      return std::make_shared<const PropertyChange<bool>>(*shown, value->target.baseVersion);
    };
    endpoint->write = [field, read = endpoint->read](bool value) -> CheckBoxResult {
      auto before = read();
      if (!before) {
        return std::unexpected(before.error());
      }
      auto written = field.set(value);
      if (!written) {
        return std::unexpected(written.error());
      }
      if (*written != EditStatus::ACCEPTED && *written != EditStatus::CLEAN && *written != EditStatus::PENDING &&
          *written != EditStatus::DIRTY) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      auto after = read();
      return after ? CheckBoxResult(PropertyUpdate<bool>{*after, (*before)->value != (*after)->value})
                   : std::unexpected(after.error());
    };
    auto changes = std::make_shared<ManagedScene::ChangeChain>();
    changes->boolean = true;
    auto binding = bindCheckbox(scene.app_.tree(), scene.app_.router(), node, std::move(endpoint),
                                scene.checkboxCallback<bool>(node, changes, {}));
    if (!binding) {
      return std::unexpected(portals::ui::detail::error(node, binding.error()));
    }
    (*entry)->binding.template emplace<CheckBoxBinding>(std::move(*binding));
    (*entry)->changes = std::move(changes);
    auto position = field.state_->current();
    if (!position) {
      return std::unexpected(error(node, position.error()));
    }
    auto target = field.state_->entry(field.definition_, *position);
    if (!target) {
      return std::unexpected(error(node, target.error()));
    }
    if (!(*target)->endpoint->capabilities().writable) {
      auto disabled = scene.app_.tree().setEnabled(node, false);
      if (!disabled) {
        return std::unexpected(error(node, disabled.error()));
      }
    }
    return {};
  }
};

template <class T> struct RowAssignment {
  Node<Widget> root;
  RowId id;
  std::shared_ptr<portals::detail::ManagedOrigin> origin;
  std::shared_ptr<const void> pin;
  const T *value{};
  std::shared_ptr<EditState<T>> edit;
  Model<T> model;
  std::function<std::expected<void, TreeError>()> erase;
  std::uint64_t valueRevision{};
  std::uint64_t snapshotRevision{};
  std::vector<Node<Label>> reactiveLabels = std::vector<Node<Label>>(0);
  bool building{};
  bool closed{};
  std::vector<std::shared_ptr<VirtualRowEditParticipant>> participants =
    std::vector<std::shared_ptr<VirtualRowEditParticipant>>(0);

  bool dirty() const {
    return std::ranges::any_of(participants,
                               [](const auto &participant) {
                                 return participant->unfinished();
                               }) ||
           (edit && std::ranges::any_of(edit->entries, [](const auto &entry) {
              return entry->hasDraft() || entry->status == EditStatus::DIRTY || entry->status == EditStatus::PENDING ||
                     entry->status == EditStatus::INVALID || entry->status == EditStatus::CONFLICT ||
                     entry->status == EditStatus::REJECTED;
            }));
  }

  // Accepted editor history belongs to its mounted lifetime. It does not consume
  // the unfinished edit pin after this assignment actually leaves the window.
  bool retainsMountedState() const {
    return dirty() || (edit && std::ranges::any_of(edit->entries, [](const auto &entry) {
                         return !entry->pristine();
                       }));
  }

  void close() noexcept {
    if (closed) {
      return;
    }
    closed = true;
    if (origin) {
      origin->closed = true;
    }
    if (edit) {
      edit->closed();
    }
    for (const auto &participant : participants) {
      participant->retire();
    }
  }
};

// Internal metadata publication runs only in the validated callback-free commit span.
struct RowsCommitPublication {
  void *context{};
  void (*publish)(void *) noexcept {};
};

template <class T> struct DeliveredRow {
  RowId id;
  const T *value{};
  std::uint64_t valueRevision{};
  std::shared_ptr<const void> pin;
};

template <class Row, class Value> class PinnedRowEndpoint final : public EditEndpoint<Value> {
public:
  PinnedRowEndpoint(std::weak_ptr<RowAssignment<Row>> row, Value Row::*member)
      : row_(std::move(row)), member_(member), owner_(std::this_thread::get_id()) {
  }

  EditCapabilities capabilities() const noexcept override {
    return {};
  }

  std::expected<EditRecord<Value>, TreeError> read(ModelResource resource, RowId id) override {
    if (owner_ != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }
    auto row = row_.lock();
    if (!row || !row->origin->check()) {
      return std::unexpected(TreeError::CLOSED);
    }
    if (row->id != id || row->edit->fixed->position.resource != resource || !member_) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    // A field may itself be a borrowed view into the accounted page. An aliasing
    // owner retains the actual immutable pin instead of making a shallow Value copy.
    return EditRecord<Value>{std::shared_ptr<const Value>(row->pin, std::addressof(row->value->*member_)),
                             row->valueRevision};
  }

  std::expected<EditReceipt, TreeError> submit(EditRequest<Value> request) override {
    return EditReceipt{request.target, EditStatus::READ_ONLY};
  }

  std::expected<std::size_t, TreeError> ownedBytes(const Value &value) const override {
    return editValueBytes(value);
  }

private:
  std::weak_ptr<RowAssignment<Row>> row_;
  Value Row::*member_;
  std::thread::id owner_;
};
template <class T, class Source, class Factory> struct RowsController;
/// @endcond
} // namespace portals::ui::detail

namespace portals::ui {
/** @brief Cold native window delivery capability, with no invented finite extent/current.
 * @tparam T Immutable arena-accounted page row type supported by WindowProvider.
 */
template <class T> struct WindowRowsModel {
  using Row = T;                               ///< Immutable domain row.
  std::shared_ptr<WindowProvider<T>> provider; ///< Worker-safe provider retained by source work.
  WindowSourceOptions options{};               ///< Existing bounded source protocol settings.
  std::shared_ptr<SourceBudget> budget; ///< Shared delivery/preparation credit root; omitted creates one at mount.
  ModelResource resource;               ///< Logical source identity, independent of per-view delivery sessions.
  std::optional<SourceError> failure;   ///< Cold descriptor construction failure, reported by checked mount.
};

/** @brief Declare native cursor delivery without claiming random access or a total row count.
 * @tparam T Immutable arena-accounted row type. @param provider Existing worker-safe window provider.
 * @param options Existing page/work bounds. @param budget Shared source credit root.
 * @return Cold distinct capability descriptor; no worker/read/scene is created.
 */
template <class T>
WindowRowsModel<T> model(std::shared_ptr<WindowProvider<T>> provider, WindowSourceOptions options = {},
                         std::shared_ptr<SourceBudget> budget = {}) {
  WindowRowsModel<T> result{std::move(provider), options, std::move(budget), {}, {}};
  auto identity = ModelResource::create();
  if (identity) {
    result.resource = *identity;
  } else {
    result.failure = identity.error();
  }
  return result;
}

/** @brief Immutable leased domain data retaining its original model range or bounded source page.
 * @tparam T Immutable domain row; reachable borrowed fields remain valid while this lease lives.
 */
template <class T> class RowSnapshot {
public:
  /// @cond INTERNAL
  RowSnapshot(std::shared_ptr<const void> pin, const T *value, RowId id)
      : pin_(std::move(pin)), value_(value), id_(id) {
  }

  /// @endcond
  /** @return Borrowed immutable value valid through this snapshot's lifetime. */
  const T &value() const noexcept {
    return *value_;
  }

  /** @return Frozen complete source row identity. */
  RowId id() const noexcept {
    return id_;
  }

private:
  std::shared_ptr<const void> pin_;
  const T *value_;
  RowId id_;
};

/** @brief Weak row-qualified field capability; saved descriptors do not keep UI assignments alive.
 * @tparam Row Domain record. @tparam Value Owning copyable field value supported by the existing edit engine.
 */
template <class Row, class Value> class RowField {
public:
  /** @brief Keep checked string descriptor transfers fallible through copying. */
  ~RowField() = default;

  /// @cond INTERNAL
  RowField(std::weak_ptr<detail::RowAssignment<Row>> row, Value Row::*member, std::thread::id owner)
      : row_(std::move(row)), member_(member), owner_(owner) {
  }
  std::expected<EditField<Row, Value>, TreeError> resolve() const try {
    if (owner_ != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }
    auto row = row_.lock();
    if (!row || !row->origin->check()) {
      return std::unexpected(TreeError::CLOSED);
    }
    EditSession<Row> session;
    session.state_ = row->edit;
    if (persistent_ && key_.empty()) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    auto field = persistent_ ? session.field(key_, member_, endpoint_) : session.field(member_);
    if (!field.state_ || !field.definition_) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    if (!row->edit->fixed->source && !field.definition_->endpoint) {
      field.definition_->endpoint = std::make_shared<detail::PinnedRowEndpoint<Row, Value>>(row_, member_);
    }
    if (validation_) {
      field = field.validate(*validation_);
    }
    return field;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }

  std::expected<void, ManagedError> attachCaption(ManagedScene &scene, Node<Label> node,
                                                  std::shared_ptr<detail::PreparedCaptionSource> source) const {
    if (owner_ != std::this_thread::get_id()) {
      return std::unexpected(detail::error(node, TreeError::WRONG_THREAD));
    }
    auto row = row_.lock();
    if (!row || !row->origin->check()) {
      return std::unexpected(detail::error(node, TreeError::CLOSED));
    }
    if (row->reactiveLabels.size() >= row->edit->options.maxRetainedFields) {
      return std::unexpected(detail::error(node, TreeError::NODE_LIMIT));
    }
    row->reactiveLabels.push_back(node);
    return detail::RowAccess::bindCaption(scene, node, std::move(source));
  }

  /// @endcond
  /** @return Existing owning accepted/draft snapshot for this exact row, or checked lifetime/thread failure.
   * Native readonly accepted values retain their actual immutable source-page lease and
   * its source credit until the last snapshot releases it; they do not retain the UI assignment.
   */
  std::expected<EditFieldSnapshot<Value>, TreeError> snapshot() const {
    auto field = resolve();
    return field ? field->snapshot() : std::unexpected(field.error());
  }

  /** @param value Owning field candidate. @return Existing edit admission/status, never retargeted through current. */
  std::expected<EditStatus, TreeError> set(const Value &value) const {
    auto field = resolve();
    return field ? field->set(value) : std::unexpected(field.error());
  }

  /** @param rule Owning synchronous validation rule. @return Copy of this weak field capability with that rule. */
  RowField validate(std::function<bool(const Value &)> rule) const {
    auto copy = *this;
    copy.validation_ = std::make_shared<const std::function<bool(const Value &)>>(std::move(rule));
    return copy;
  }

private:
  template <class> friend class RowContext;
  std::weak_ptr<detail::RowAssignment<Row>> row_;
  Value Row::*member_{};
  std::thread::id owner_;
  std::string key_ = std::string(0, '\0');
  bool persistent_{};
  std::shared_ptr<EditEndpoint<Value>> endpoint_;
  std::shared_ptr<const std::function<bool(const Value &)>> validation_;
};

namespace detail {
/// @cond INTERNAL
template <class Row, class Value> struct FieldCaptionSource final : PreparedCaptionSource {
  explicit FieldCaptionSource(EditField<Row, Value> value) : field(std::move(value)) {
  }

  EditField<Row, Value> field;
  std::shared_ptr<const PropertyChange<std::string>> cached;

  std::expected<std::shared_ptr<const PropertyChange<std::string>>, TreeError> snapshot() override {
    auto position = field.state_->current();
    if (!position) {
      return std::unexpected(position.error());
    }
    auto entry = field.state_->entry(field.definition_, *position);
    if (!entry) {
      return std::unexpected(entry.error());
    }
    auto ready = (*entry)->refresh();
    if (!ready) {
      if (cached && ready.error() == TreeError::WOULD_BLOCK) {
        return cached;
      }
      return std::unexpected(ready.error());
    }
    if (cached && cached->revision == (*entry)->observedVersion) {
      return cached;
    }
    auto text = Access::formatValueCaption(*(*entry)->accepted);
    if (!text) {
      return std::unexpected(text.error());
    }
    auto next = std::make_shared<const PropertyChange<std::string>>(*text, (*entry)->observedVersion);
    cached.swap(next);
    return cached;
  }
};

/// @endcond
} // namespace detail

/** @brief Weak exact-record factory/action context; it never follows a recycled position.
 * @tparam T Immutable domain row type. Saved handles close when their assignment retires.
 */
template <class T> class RowContext {
public:
  /** @brief Empty weak row handle; checked operations report CLOSED. */
  RowContext() = default;

  /// @cond INTERNAL
  explicit RowContext(std::weak_ptr<detail::RowAssignment<T>> assignment)
      : assignment_(std::move(assignment)), owner_(std::this_thread::get_id()) {
  }

  /// @endcond
  /** @return Complete row identity copied from the assignment, or zero after retirement. */
  RowId id() const noexcept {
    if (owner_ != std::this_thread::get_id()) {
      return {};
    }
    auto row = assignment_.lock();
    return row ? row->id : RowId{};
  }

  /** @return Factory-only borrowed immutable row, valid until the factory returns.
   * @throws TreeError CLOSED after retirement; factory exceptions become checked mount failures.
   */
  const T &value() const {
    if (owner_ != std::this_thread::get_id()) {
      throw TreeError::WRONG_THREAD;
    }
    auto row = assignment_.lock();
    if (!row || !row->origin->check()) {
      throw TreeError::CLOSED;
    }
    if (!row->building) {
      throw TreeError::WOULD_BLOCK;
    }
    return *row->value;
  }

  /** @tparam Value Owning field value. @param member Non-null data member pointer.
   * @return Existing fixed-target edit field, using the shared edit/history/validation engine.
   */
  template <class Value, class Record>
    requires std::same_as<T, Record>
  RowField<T, Value> field(Value Record::*member) const {
    return RowField<T, Value>(assignment_, member, owner_);
  }

  /** @tparam Value Owning supported field type. @param key Persistent schema field key.
   * @param member Selected field. @param endpoint Existing exact-target backend write/read capability.
   * @return Weak field capability using the same edit protocol as finite models.
   */
  template <class Value, class Record>
    requires std::same_as<T, Record>
  RowField<T, Value> field(std::string_view key, Value Record::*member,
                           std::type_identity_t<std::shared_ptr<EditEndpoint<Value>>> endpoint) const {
    auto result = field(member);
    result.key_ = key;
    result.persistent_ = true;
    result.endpoint_ = std::move(endpoint);
    return result;
  }

  /** @return Checked owning data lease for later callback use; retired assignments return CLOSED. */
  std::expected<RowSnapshot<T>, TreeError> snapshot() const {
    if (owner_ != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }
    auto row = assignment_.lock();
    if (!row || !row->origin->check()) {
      return std::unexpected(TreeError::CLOSED);
    }
    return RowSnapshot<T>(row->pin, row->value, row->id);
  }

  /** @return Checked commit of this row's retained fields through the existing edit session. */
  std::expected<void, TreeError> commit() const {
    if (owner_ != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }
    auto row = assignment_.lock();
    if (!row || !row->origin->check()) {
      return std::unexpected(TreeError::CLOSED);
    }
    EditSession<T> session;
    session.state_ = row->edit;
    return session.commitChecked();
  }

  /** @return Checked cancellation of this row's local drafts; admitted backend work remains admitted. */
  std::expected<void, TreeError> cancel() const {
    if (owner_ != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }
    auto row = assignment_.lock();
    if (!row || !row->origin->check()) {
      return std::unexpected(TreeError::CLOSED);
    }
    EditSession<T> session;
    session.state_ = row->edit;
    return session.cancelChecked();
  }

  /** @param receipt Original exact-target backend acknowledgement, delivered through the existing UI path.
   * @return True when applied, false after retirement/stale completion, or a checked thread/protocol failure.
   */
  std::expected<bool, TreeError> applyReply(const EditReceipt &receipt) const {
    if (owner_ != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }
    auto row = assignment_.lock();
    if (!row || !row->origin->check()) {
      return false;
    }
    EditSession<T> session;
    session.state_ = row->edit;
    auto applied = session.applyReply(receipt);
    if (applied && *applied && row->origin->check()) {
      auto invalidated = row->origin->tree->invalidate(row->root, Invalidation::LAYOUT);
      if (!invalidated) {
        return std::unexpected(invalidated.error());
      }
    }
    return applied;
  }

  /** @return Pre-mutation admission for a custom editor's unfinished state, sharing the one-row pin budget. */
  std::expected<void, TreeError> admitEdit() const {
    if (owner_ != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }
    auto row = assignment_.lock();
    if (!row || !row->origin->check()) {
      return std::unexpected(TreeError::CLOSED);
    }
    return row->edit->fixed->admitDraft();
  }
  /** @param participant Typed custom state, owning any unfinished draft/history/selection.
   * @return Bounded UI-affine registration, or checked lifetime/capacity/allocation failure.
   * The custom control must call admitEdit before changing unfinished state.
   */
  std::expected<void, TreeError> participate(std::shared_ptr<VirtualRowEditParticipant> participant) const try {
    if (owner_ != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }
    auto row = assignment_.lock();
    if (!row || !row->origin->check()) {
      return std::unexpected(TreeError::CLOSED);
    }
    if (!participant) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    if (std::ranges::find(row->participants, participant) != row->participants.end()) {
      return {};
    }
    if (row->participants.size() >= row->edit->options.maxRetainedFields) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    row->participants.push_back(std::move(participant));
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }

  /** @return Exact-record erase admission, or CLOSED/unsupported/missing-target failure. */
  std::expected<void, TreeError> erase() const {
    if (owner_ != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }
    auto row = assignment_.lock();
    if (!row || !row->origin->check()) {
      return std::unexpected(TreeError::CLOSED);
    }
    return row->erase ? row->erase() : std::unexpected(TreeError::INVALID_ARGUMENT);
  }

private:
  std::weak_ptr<detail::RowAssignment<T>> assignment_;
  std::thread::id owner_{std::this_thread::get_id()};
};

/** @brief Bind a real checkbox directly to the existing exact-record field endpoint.
 * @tparam Row Domain record type. @param text Owning caption copy. @param field Fixed or current-record bool field.
 * @param style Manual appearance. @param policy Layout bounds.
 * @return Checked recipe; rejected writes leave the displayed checked state unchanged.
 */
template <class Row>
Description<CheckBox> checkbox(std::string_view text, EditField<Row, bool> field, ControlStyle style = {},
                               SizePolicy policy = DEFAULT_CONTENT_POLICY) {
  return detail::Access::controlSetup(
    detail::Access::controlArguments(control<CheckBox>(text, style, policy),
                                     {.text = TextValue(text), .cycle = CheckCycle::TWO_STATE}),
    [field = std::move(field)](ManagedScene &scene, Node<CheckBox> node,
                               const ControlSettings &settings) mutable -> std::expected<void, ManagedError> {
      if (!field.state_ || !field.definition_ || settings.state == CheckState::MIXED ||
          settings.cycle == CheckCycle::THREE_STATE) {
        return std::unexpected(detail::error(node, TreeError::INVALID_ARGUMENT));
      }
      detail::MountContext context;
      auto resolved = field.state_->resolve(scene, context);
      if (!resolved) {
        return std::unexpected(detail::error(node, resolved.error()));
      }
      auto bound = detail::RowAccess::boolean(scene, node, std::move(field));
      if (bound) {
        detail::ModelAccess::publish(context);
      }
      return bound;
    });
}

/** @tparam Row Domain record. @param text Caption copy. @param field Weak exact-row bool binding.
 * @param style Appearance. @param policy Layout bounds. @return Ordinary real checkbox recipe.
 */
template <class Row>
Description<CheckBox> checkbox(std::string_view text, RowField<Row, bool> field, ControlStyle style = {},
                               SizePolicy policy = DEFAULT_CONTENT_POLICY) {
  auto resolved = field.resolve();
  return checkbox(text, resolved ? *resolved : EditField<Row, bool>{}, style, policy);
}

/** @brief Display accepted row-field values through the existing managed caption synchronization.
 * @tparam Row Domain record. @tparam Value Owning supported field value formattable as text.
 * @param field Weak exact-row field capability. @param style Appearance. @param policy Layout bounds.
 * @return Reactive accepted-value label; dirty drafts remain local to their editor.
 */
template <class Row, class Value>
  requires(std::formattable<Value, char> || std::same_as<Value, GraphemeText>)
Description<Label> label(RowField<Row, Value> field, ControlStyle style = {},
                         SizePolicy policy = DEFAULT_CONTENT_POLICY) {
  return detail::Access::controlSetup(
    control<Label>("", style, policy),
    [field](ManagedScene &scene, Node<Label> node, const ControlSettings &) -> std::expected<void, ManagedError> {
      auto resolved = field.resolve();
      if (!resolved) {
        return std::unexpected(detail::error(node, resolved.error()));
      }
      auto source = std::make_shared<detail::FieldCaptionSource<Row, Value>>(*resolved);
      return field.attachCaption(scene, node, std::move(source));
    });
}

/** @tparam Row Domain record. @param field Weak exact-row string binding. @param config Existing editor rules.
 * @return Ordinary real LineEdit recipe using the common edit/history/validation runtime.
 */
template <class Row> Description<LineEdit> lineEdit(RowField<Row, std::string> field, LineEditConfig config = {}) {
  auto resolved = field.resolve();
  return lineEdit(resolved ? *resolved : EditField<Row, std::string>{}, std::move(config));
}

/** @tparam Row Domain record. @param caption Field caption. @param field Weak exact-row string binding.
 * @param config Existing validation/editor configuration. @return Ordinary model-bound FormField recipe.
 */
template <class Row>
Description<FormField> formField(std::string_view caption, RowField<Row, std::string> field,
                                 FormFieldConfig config = {}) {
  auto resolved = field.resolve();
  return formField(caption, resolved ? *resolved : EditField<Row, std::string>{}, std::move(config));
}

/** @tparam Row Domain record. @tparam Value Owning supported field value.
 * @param field Weak exact-row capability. @param codec Existing owning scalar text conversion.
 * @param config Existing editor rules. @return Existing model-bound LineEdit with the supplied codec.
 */
template <class Row, class Value>
Description<LineEdit> lineEdit(RowField<Row, Value> field,
                               std::type_identity_t<std::shared_ptr<const EditTextCodec<Value>>> codec,
                               LineEditConfig config = {}) {
  auto resolved = field.resolve();
  return lineEdit(resolved ? *resolved : EditField<Row, Value>{}, std::move(codec), std::move(config));
}

/** @tparam Row Domain record. @tparam Value Owning supported field value.
 * @param caption Field caption. @param field Weak exact-row capability. @param codec Existing scalar text conversion.
 * @param config Existing validation/editor rules. @return Existing model-bound FormField with the supplied codec.
 */
template <class Row, class Value>
Description<FormField> formField(std::string_view caption, RowField<Row, Value> field,
                                 std::type_identity_t<std::shared_ptr<const EditTextCodec<Value>>> codec,
                                 FormFieldConfig config = {}) {
  auto resolved = field.resolve();
  return formField(caption, resolved ? *resolved : EditField<Row, Value>{}, std::move(codec), std::move(config));
}
} // namespace portals::ui

#include <portals/detail/ui_virtual_rows.hpp>

namespace portals::ui {
template <class T>
template <class Row>
  requires std::same_as<T, VirtualRows<Row>>
std::expected<void, ManagedError> Ref<T>::setModel(Model<Row> source, ModelRebindOptions options,
                                                   std::source_location origin) const {
  auto owner = detail::eventScene(target());
  if (!owner) {
    return std::unexpected(owner.error());
  }
  std::expected<void, TreeError> result;
  auto edited = detail::RowAccess::tree(**owner).edit(this->node(), [&](auto &host) {
    auto *binding = dynamic_cast<portals::detail::VirtualRowsModelBinding<Row> *>(host.state().get());
    result = binding ? binding->replaceModel(std::move(source), options) : std::unexpected(TreeError::INVALID_ARGUMENT);
  });
  if (!edited || !result) {
    auto error = detail::error(this->node(), !edited ? edited.error() : result.error());
    error.phase = ManagedPhase::BIND;
    error.registrationOrigin = origin;
    return std::unexpected(error);
  }
  return {};
}

template <class T>
template <class Row>
  requires std::same_as<T, VirtualRows<Row>>
bool Ref<T>::model(Model<Row> source, ModelRebindOptions options, std::source_location origin) const {
  auto result = setModel(std::move(source), options, origin);
  if (!result) {
    detail::reportEventError(target(), result.error(), origin);
  }
  return result.has_value();
}

template <class T>
template <class Row>
  requires std::same_as<T, VirtualRows<Row>>
std::expected<void, ManagedError> Ref<T>::setModel(const WindowRowsModel<Row> &source, ModelRebindOptions options,
                                                   std::source_location origin) const {
  auto owner = detail::eventScene(target());
  if (!owner) {
    return std::unexpected(owner.error());
  }
  std::expected<void, TreeError> result;
  std::optional<SourceError> sourceError;
  auto edited = detail::RowAccess::tree(**owner).edit(this->node(), [&](auto &host) {
    auto *binding = dynamic_cast<portals::detail::VirtualRowsModelBinding<Row> *>(host.state().get());
    result = binding ? binding->replaceWindowModel(source, options) : std::unexpected(TreeError::INVALID_ARGUMENT);
    sourceError = host.sourceError();
  });
  if (!edited || !result) {
    auto error = detail::error(this->node(), !edited ? edited.error() : result.error());
    error.phase = ManagedPhase::BIND;
    error.source = sourceError;
    error.registrationOrigin = origin;
    return std::unexpected(error);
  }
  return {};
}

template <class T>
template <class Row>
  requires std::same_as<T, VirtualRows<Row>>
bool Ref<T>::model(const WindowRowsModel<Row> &source, ModelRebindOptions options, std::source_location origin) const {
  auto result = setModel(source, options, origin);
  if (!result) {
    detail::reportEventError(target(), result.error(), origin);
  }
  return result.has_value();
}
} // namespace portals::ui
