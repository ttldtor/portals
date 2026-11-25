// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_model.hpp>

#include <ranges>

namespace portals::ui {
namespace detail {
/// @cond INTERNAL
inline void reportCaptionModelError(const std::shared_ptr<RefTarget> &target, ManagedError &failure,
                                    std::source_location origin, const char *control) noexcept {
  Access::reportValueError(target, failure, origin, "model", control);
}

template <class T> inline constexpr bool IS_PROVIDER_MODEL_INPUT = false;
template <class T> inline constexpr bool IS_PROVIDER_MODEL_INPUT<std::shared_ptr<const ModelSnapshot<T>>> = true;
template <class T> inline constexpr unsigned char VALUE_CAPTION_TYPE{};

template <std::equality_comparable T> struct ScalarModelDeclaration : ModelDeclarationBase {
  explicit ScalarModelDeclaration(Kind kind) : ModelDeclarationBase(kind) {
  }

  ManagedState<T> handle;
};

template <std::equality_comparable T> struct ValueModelDeclaration : ScalarModelDeclaration<T> {
  explicit ValueModelDeclaration(const T &value)
      : ScalarModelDeclaration<T>(ModelDeclarationBase::Kind::VALUE), initial(std::make_shared<const T>(value)) {
  }

  std::shared_ptr<const T> initial;
};

template <std::equality_comparable T> struct AliasModelDeclaration : ScalarModelDeclaration<T> {
  AliasModelDeclaration(ManagedState<T> state, ModelDeclarationBase::Kind kind) : ScalarModelDeclaration<T>(kind) {
    this->handle = std::move(state);
  }
};

template <std::equality_comparable T> struct DeferredSourceDeclaration : ScalarModelDeclaration<T> {
  using Factory = std::function<std::expected<ManagedState<T>, ManagedError>(ManagedScene &, MountContext &)>;

  explicit DeferredSourceDeclaration(Factory resolver)
      : ScalarModelDeclaration<T>(ModelDeclarationBase::Kind::VALUE_DEFERRED_SOURCE), resolve(std::move(resolver)) {
  }

  Factory resolve;
};

struct ValueModelAccess {
  template <std::equality_comparable T>
  static Model<T> deferred(typename DeferredSourceDeclaration<T>::Factory factory) {
    try {
      Model<T> result;

      result.data_ = std::make_shared<DeferredSourceDeclaration<T>>(std::move(factory));

      return result;
    } catch (const std::bad_alloc &) {
      return ModelAccess::failed<T>(TreeError::NO_MEMORY);
    } catch (...) {
      return ModelAccess::failed<T>(TreeError::CALLBACK_FAILED);
    }
  }

  template <class T> static std::shared_ptr<void> identity(const Model<T> &model) {
    return ModelAccess::identity(model);
  }

  template <class T> static Model<T> root(const ModelValue<T> &value) {
    Model<T> result;

    result.data_ = value.data_;
    result.failure_ = value.failure_;

    return result;
  }

  template <class T, class Member> static Model<T> root(const ModelField<T, Member> &field) {
    return field.root_;
  }

  template <class T, class Member> static auto member(const ModelField<T, Member> &field) {
    return field.member_;
  }

  template <std::equality_comparable T> static Model<T> make(const T &initial) {
    try {
      Model<T> result;

      result.data_ = std::make_shared<ValueModelDeclaration<T>>(initial);

      return result;
    } catch (const std::bad_alloc &) {
      return ModelAccess::failed<T>(TreeError::NO_MEMORY);
    } catch (...) {
      return ModelAccess::failed<T>(TreeError::CALLBACK_FAILED);
    }
  }

  static bool isScalarKind(const std::shared_ptr<ModelDeclarationBase> &base) {
    return base &&
           (base->kind == ModelDeclarationBase::Kind::VALUE || base->kind == ModelDeclarationBase::Kind::VALUE_ALIAS ||
            base->kind == ModelDeclarationBase::Kind::VALUE_SOURCE ||
            base->kind == ModelDeclarationBase::Kind::VALUE_DEFERRED_SOURCE);
  }

  template <std::equality_comparable T> static Model<T> alias(ManagedState<T> state, bool writable) {
    try {
      Model<T> result;

      result.data_ = std::make_shared<AliasModelDeclaration<T>>(std::move(state),
                                                                writable ? ModelDeclarationBase::Kind::VALUE_ALIAS
                                                                         : ModelDeclarationBase::Kind::VALUE_SOURCE);

      return result;
    } catch (const std::bad_alloc &) {
      return ModelAccess::failed<T>(TreeError::NO_MEMORY);
    } catch (...) {
      return ModelAccess::failed<T>(TreeError::CALLBACK_FAILED);
    }
  }

  template <class T> static Model<NavigationPosition> current(const Navigation<T> &navigation) {
    try {
      Model<NavigationPosition> result;

      result.data_ = std::make_shared<DeferredSourceDeclaration<NavigationPosition>>(
        [navigation](ManagedScene &scene,
                     MountContext &context) -> std::expected<ManagedState<NavigationPosition>, ManagedError> {
          auto resolved = ModelAccess::resolve(scene, navigation, context);
          if (!resolved) {
            return std::unexpected(resolved.error());
          }

          auto source = resolved->current();
          if (!source) {
            return std::unexpected(ManagedError{ManagedPhase::STATE, scene.root().id(), source.error(), {}, {}});
          }

          return Access::valueState(*source);
        });

      return result;
    } catch (const std::bad_alloc &) {
      return ModelAccess::failed<NavigationPosition>(TreeError::NO_MEMORY);
    } catch (...) {
      return ModelAccess::failed<NavigationPosition>(TreeError::CALLBACK_FAILED);
    }
  }

  template <std::equality_comparable T>
  static std::expected<ManagedState<T>, ManagedError> state(const std::shared_ptr<ModelDeclarationBase> &base,
                                                            TreeError failure, bool writable = false) {
    if (!isScalarKind(base)) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, {}, failure, {}, {}});
    }

    if (base->observation->owner != std::this_thread::get_id()) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::WRONG_THREAD, {}, {}});
    }
    if (!base->published) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::CLOSED, {}, {}});
    }
    auto owner = eventScene(base->observation);
    if (!owner) {
      return std::unexpected(owner.error());
    }
    if (Access::modelSceneClosed(**owner)) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, base->scene, TreeError::CLOSED, {}, {}});
    }

    const auto data = std::static_pointer_cast<ScalarModelDeclaration<T>>(base);
    auto qualified = Access::validateValueState(**owner, data->handle);
    if (!qualified) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, base->scene, qualified.error(), {}, {}});
    }
    if (writable && base->kind != ModelDeclarationBase::Kind::VALUE &&
        base->kind != ModelDeclarationBase::Kind::VALUE_ALIAS) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, base->scene, TreeError::INVALID_ARGUMENT, {}, {}});
    }

    return data->handle;
  }

  template <std::equality_comparable T>
  static std::expected<ManagedState<T>, ManagedError> resolve(ManagedScene &scene, const Model<T> &value,
                                                              MountContext &context) {
    const auto base = value.data_;
    const auto root = scene.root();
    const auto owner = Access::lifetime(scene);
    const auto fail = [root](TreeError error) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, root.id(), error, {}, {}});
    };

    if (Access::modelSceneClosed(scene)) {
      return fail(TreeError::CLOSED);
    }
    if (!isScalarKind(base)) {
      return fail(value.failure_);
    }
    if (base->observation->owner != std::this_thread::get_id()) {
      return fail(TreeError::WRONG_THREAD);
    }

    const auto data = std::static_pointer_cast<ScalarModelDeclaration<T>>(base);
    if (data->resolving) {
      return fail(TreeError::WOULD_BLOCK);
    }
    if (data->scene != NodeId{}) {
      if (data->scene != root.id()) {
        return fail(TreeError::WRONG_SESSION);
      }
      if (data->published) {
        return state<T>(base, value.failure_);
      }

      if (data->attempt != &context) {
        return fail(TreeError::WOULD_BLOCK);
      }

      auto qualified = Access::validateValueState(scene, data->handle);

      return qualified ? std::expected<ManagedState<T>, ManagedError>(data->handle)
                       : std::expected<ManagedState<T>, ManagedError>(fail(qualified.error()));
    }

    ModelDeclarationOriginScope ownership(scene, *data->observation);
    if (ownership.failure) {
      return fail(*ownership.failure);
    }

    struct Resolving {
      bool &active;

      ~Resolving() {
        active = false;
      }
    } guard{data->resolving};

    data->resolving = true;
    try {
      std::expected<ManagedState<T>, ManagedError> created = data->handle;
      if (data->kind == ModelDeclarationBase::Kind::VALUE) {
        created = Access::createValueState(scene, *std::static_pointer_cast<ValueModelDeclaration<T>>(base)->initial);
      } else if (data->kind == ModelDeclarationBase::Kind::VALUE_DEFERRED_SOURCE) {
        created = std::static_pointer_cast<DeferredSourceDeclaration<T>>(base)->resolve(scene, context);
      }

      if (owner.expired()) {
        return fail(TreeError::CLOSED);
      }
      if (!created) {
        return std::unexpected(created.error());
      }

      auto qualified = Access::validateValueState(scene, *created);
      if (!qualified) {
        return fail(qualified.error());
      }

      const bool ownsState = data->kind == ModelDeclarationBase::Kind::VALUE;
      bool rollbackInstalled{};
      try {
        context.rollback.emplace_back([&scene, data, owner, ownsState, handle = *created]() mutable {
          if (ownsState || data->kind == ModelDeclarationBase::Kind::VALUE_DEFERRED_SOURCE) {
            data->handle = {};
          }
          data->scene = {};
          data->attempt = nullptr;
          if (ownsState && !owner.expired()) {
            Access::retireValueState(scene, handle);
          }
        });
        rollbackInstalled = true;
        context.publish.emplace_back([&scene, data, root, owner] {
          data->observation->scene = &scene;
          data->observation->lifetime = owner;
          data->observation->node = root;
          data->attempt = nullptr;
          data->published = true;
        });
      } catch (...) {
        if (ownsState && !rollbackInstalled) {
          Access::retireValueState(scene, *created);
        }
        throw;
      }

      data->handle = *created;
      data->scene = root.id();
      data->attempt = &context;

      return *created;
    } catch (const std::bad_alloc &) {
      return fail(owner.expired() ? TreeError::CLOSED : TreeError::NO_MEMORY);
    } catch (...) {
      return fail(owner.expired() ? TreeError::CLOSED : TreeError::CALLBACK_FAILED);
    }
  }

  template <std::equality_comparable T, class Projection, class Receiver>
  static std::expected<void, ManagedError> bindCaption(ManagedScene &scene, Node<Receiver> node, const Model<T> &source,
                                                       const std::shared_ptr<Projection> &projection,
                                                       MountContext &context, const void *previous = nullptr) {
    const auto owner = Access::lifetime(scene);
    auto model = resolve(scene, source, context);
    if (!model) {
      return std::unexpected(model.error());
    }

    auto metadata = std::make_shared<ValueCaptionBinding>();

    metadata->source = source.data_;
    metadata->type = &VALUE_CAPTION_TYPE<T>;
    metadata->bind = std::make_shared<const ValueCaptionBinding::Bind>(
      [projection, node](ManagedScene &scene, std::shared_ptr<void> base, MountContext &context,
                         const void *previous) -> std::expected<void, ManagedError> {
        Model<T> replacement;

        replacement.data_ = std::static_pointer_cast<ModelDeclarationBase>(base);

        return bindCaption(scene, node, replacement, projection, context, previous);
      });

    ManagedState<std::string> text;
    if constexpr (std::same_as<T, std::string> && std::same_as<Projection, std::identity>) {
      text = *model;
    } else {
      auto graph = std::make_shared<Access::ValueCaptionGraph>();

      graph->scene = &scene;
      graph->lifetime = owner;
      metadata->graph = graph;

      auto projected = Access::projectValueCaption(scene, node, *model, projection, graph);
      if (!projected) {
        return std::unexpected(projected.error());
      }

      text = *projected;
    }

    if (owner.expired()) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::CLOSED, {}, {}});
    }

    return Access::installValueCaption(scene, node, text, std::move(metadata), previous, context);
  }

  template <std::equality_comparable T, class Receiver>
  static std::expected<void, ManagedError> replaceCaption(ManagedScene &scene, Node<Receiver> node,
                                                          const Model<T> &source) {
    const auto owner = Access::lifetime(scene);
    auto slot = Access::valueCaptionSlot(scene, node);
    if (!slot) {
      return std::unexpected(slot.error());
    }
    if (!isScalarKind(source.data_)) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), source.failure_, {}, {}});
    }
    if (source.data_->observation->owner != std::this_thread::get_id()) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::WRONG_THREAD, {}, {}});
    }
    if (slot->source && slot->source->type != &VALUE_CAPTION_TYPE<T>) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }
    if (slot->source && slot->source->source == source.data_) {
      auto qualified = state<T>(source.data_, source.failure_);

      return qualified ? std::expected<void, ManagedError>{} : std::unexpected(qualified.error());
    }

    MountContext context;
    std::expected<void, ManagedError> bound;

    if (slot->source) {
      // Pin the executing factory and its projection across callback-copy/destructor callouts.
      const auto factory = slot->source->bind;

      bound = (*factory)(scene, source.data_, context, slot->identity);
    } else if constexpr (std::formattable<T, char> || std::same_as<T, GraphemeText>) {
      bound = bindCaption(scene, node, source, std::make_shared<std::identity>(), context, slot->identity);
    } else {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }

    if (!bound) {
      return bound;
    }
    if (owner.expired()) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::CLOSED, {}, {}});
    }

    ModelAccess::publish(context);

    return {};
  }
};

/// @endcond
} // namespace detail

/** @brief Weak scalar property observation; assignment publishes a value without rebinding the descriptor.
 * @tparam T Owning equality-comparable scalar value; instantiated only for the scalar capability.
 * Checked operations never collect errors. Assignment reports an exact failure through the existing
 * scene collector and returns its immutable operation receipt. Postcommit notification rejection
 * remains a successful PropertyUpdate with notification_error; the committed value is not rolled back.
 */
template <class T> class ModelValue {
public:
  /** @param other Observation copied without reading or publishing values. */
  ModelValue(const ModelValue &other) = default;

  /** @return Independent immutable value/revision owner or the original affinity/lifetime/capability error. */
  std::expected<std::shared_ptr<const PropertyChange<T>>, ManagedError> snapshot() const {
    const auto base = data_;
    auto state = detail::ValueModelAccess::state<T>(base, failure_);
    if (!state) {
      return std::unexpected(state.error());
    }

    const auto node = base->scene;
    auto read = state->snapshot();

    return read ? std::expected<std::shared_ptr<const PropertyChange<T>>, ManagedError>(*read)
                : std::unexpected(ManagedError{ManagedPhase::STATE, node, read.error(), {}, {}});
  }

  /** @param value Candidate copied by the existing Property transaction after validation.
   * @return Exact commit/delivery receipt or checked rejection; no diagnostic is collected.
   * Equal values are silent. Read-only aliases return INVALID_ARGUMENT without constructing a candidate.
   * Value construction/comparison must satisfy Property's non-reentry contract.
   */
  std::expected<PropertyUpdate<T>, ManagedError> set(const T &value) const {
    const auto base = data_;
    auto state = detail::ValueModelAccess::state<T>(base, failure_, true);
    if (!state) {
      return std::unexpected(state.error());
    }

    const auto node = base->scene;
    auto changed = state->set(value);

    return changed ? std::expected<PropertyUpdate<T>, ManagedError>(*changed)
                   : std::unexpected(ManagedError{ManagedPhase::STATE, node, changed.error(), {}, {}});
  }

  /** @param value Replacement copied through checked set; errors also reach the existing collector.
   * @return Owning observation of this exact operation; this original proxy's saved receipt is unchanged.
   */
  ModelValue operator=(const T &value) {
    ModelValue result(*this);
    const auto observation = data_ ? data_->observation : nullptr;
    const auto origin = origin_;

    result.receipt_ = result.set(value);
    if (!*result.receipt_) {
      detail::Access::reportValueError(observation, result.receipt_->error(), origin);
    }

    return result;
  }

  /** @param source Source read once before assignment; no lasting binding or descriptor is copied.
   * @return Exact assignment observation; read/write failures also reach the existing collector.
   */
  ModelValue operator=(const ModelValue &source) {
    ModelValue result(*this);
    const auto observation = data_ ? data_->observation : nullptr;
    const auto origin = origin_;
    auto value = source.snapshot();

    result.receipt_ = value ? result.set((*value)->value)
                            : std::expected<PropertyUpdate<T>, ManagedError>(std::unexpected(value.error()));
    if (!*result.receipt_) {
      detail::Access::reportValueError(observation, result.receipt_->error(), origin);
    }

    return result;
  }

  /** @return This assignment's commit receipt, or INVALID_ARGUMENT before assignment; never reads a scene. */
  std::expected<PropertyUpdate<T>, ManagedError> result() const {
    return receipt_ ? *receipt_
                    : std::expected<PropertyUpdate<T>, ManagedError>(
                        std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::INVALID_ARGUMENT, {}, {}}));
  }

private:
  friend class Model<T>;
  friend struct detail::ValueModelAccess;

  ModelValue(std::shared_ptr<detail::ModelDeclarationBase> data, TreeError failure, std::source_location origin)
      : data_(std::move(data)), failure_(failure), origin_(origin) {
  }

  std::shared_ptr<detail::ModelDeclarationBase> data_;
  TreeError failure_;
  std::source_location origin_;
  std::optional<std::expected<PropertyUpdate<T>, ManagedError>> receipt_;
};

template <class T>
ModelValue<T> Model<T>::value(std::source_location origin) const
  requires std::equality_comparable<T>
{
  return ModelValue<T>(data_, failure_, origin);
}

/** @brief Owning field descriptor over one scalar root; snapshots never retain a borrowed member reference.
 * @tparam T Copyable equality-comparable owning record, under Property's non-reentry contract.
 * @tparam Member Owning data-member type; const members do not offer set or assignment.
 * Reads copy the selected field and retain the root revision. Writes copy the current root, replace
 * this field and use its existing transaction; they are UI-affine, not external conditional writes.
 * Receipts are the exact root PropertyUpdate<T>, including postcommit notification rejection.
 */
template <class T, class Member> class ModelField {
public:
  using Value = std::remove_cv_t<Member>; ///< Owning projected value, without the member's const qualification.

  /** @param other Descriptor copied without reading or publishing values. */
  ModelField(const ModelField &other) = default;

  /** @return Independent selected value/root revision, or typed read/copy/allocation failure. */
  std::expected<std::shared_ptr<const PropertyChange<Value>>, ManagedError> snapshot() const {
    if (!member_) {
      return failure(TreeError::INVALID_ARGUMENT);
    }

    auto root = root_.value().snapshot();
    if (!root) {
      return std::unexpected(root.error());
    }

    try {
      return std::make_shared<const PropertyChange<Value>>((*root)->value.*member_, (*root)->revision);
    } catch (const std::bad_alloc &) {
      return failure(TreeError::NO_MEMORY);
    } catch (...) {
      return failure(TreeError::CALLBACK_FAILED);
    }
  }

  /** @param value Field candidate copied into the latest accepted root; other fields are preserved.
   * @return Exact root commit/delivery receipt, or checked failure without collecting diagnostics.
   * Copy/assignment/comparison must not reenter UI APIs, as for the root Property value.
   */
  std::expected<PropertyUpdate<T>, ManagedError> set(const Value &value) const
    requires(!std::is_const_v<Member>) && std::assignable_from<Member &, const Value &>
  {
    if (!member_) {
      return failure(TreeError::INVALID_ARGUMENT);
    }

    auto state = detail::ValueModelAccess::state<T>(root_.data_, root_.failure_, true);
    if (!state) {
      return std::unexpected(state.error());
    }

    auto root = root_.value().snapshot();
    if (!root) {
      return std::unexpected(root.error());
    }

    try {
      T candidate = (*root)->value;

      candidate.*member_ = value;

      return root_.value().set(candidate);
    } catch (const std::bad_alloc &) {
      return failure(TreeError::NO_MEMORY);
    } catch (...) {
      return failure(TreeError::CALLBACK_FAILED);
    }
  }

  /** @param value Replacement for this field; collected failures use the root model.value operation.
   * @return Owning receipt for this assignment; the original descriptor's receipt is unchanged.
   */
  ModelField operator=(const Value &value)
    requires(!std::is_const_v<Member>) && std::assignable_from<Member &, const Value &>
  {
    ModelField result(*this);
    const auto observation = root_.data_ ? root_.data_->observation : nullptr;
    const auto origin = origin_;

    result.receipt_ = result.set(value);
    if (!*result.receipt_) {
      detail::Access::reportValueError(observation, result.receipt_->error(), origin);
    }

    return result;
  }

  /** @param source Field read once and copied as a value; no lasting binding is established.
   * @return Exact root assignment receipt; read/write failures reach the existing collector.
   */
  ModelField operator=(const ModelField &source)
    requires(!std::is_const_v<Member>) && std::assignable_from<Member &, const Value &>
  {
    ModelField result(*this);
    const auto observation = root_.data_ ? root_.data_->observation : nullptr;
    const auto origin = origin_;
    auto value = source.snapshot();

    result.receipt_ = value ? result.set((*value)->value)
                            : std::expected<PropertyUpdate<T>, ManagedError>(std::unexpected(value.error()));
    if (!*result.receipt_) {
      detail::Access::reportValueError(observation, result.receipt_->error(), origin);
    }

    return result;
  }

  /** @tparam Delta Value accepted by the member type's addition operator.
   * @param delta Added to the latest accepted field, using the ordinary C++ arithmetic/type semantics.
   * @return Exact root assignment receipt, collecting checked read/computation/write failures once.
   * Other fields are preserved. No saturation or overflow policy is added to the member's own operator.
   * User operators, copies and comparisons follow Property's pure, non-reentrant value contract.
   */
  template <class Delta>
    requires(!std::is_const_v<Member>) && std::assignable_from<Member &, const Value &> &&
            requires(const Value &value, const Delta &delta) { Value(value + delta); }
  ModelField operator+=(const Delta &delta) {
    ModelField result(*this);
    const auto observation = root_.data_ ? root_.data_->observation : nullptr;
    const auto origin = origin_;
    auto writable = detail::ValueModelAccess::state<T>(root_.data_, root_.failure_, true);
    auto current = writable ? snapshot() : decltype(snapshot())(std::unexpected(writable.error()));

    if (!current) {
      result.receipt_ = std::unexpected(current.error());
    } else {
      try {
        const Value candidate((*current)->value + delta);

        result.receipt_ = result.set(candidate);
      } catch (const std::bad_alloc &) {
        result.receipt_ = failure(TreeError::NO_MEMORY);
      } catch (...) {
        result.receipt_ = failure(TreeError::CALLBACK_FAILED);
      }
    }
    if (!*result.receipt_) {
      detail::Access::reportValueError(observation, result.receipt_->error(), origin);
    }

    return result;
  }

  /** @return Saved root assignment receipt, or INVALID_ARGUMENT before assignment; never reads a scene. */
  std::expected<PropertyUpdate<T>, ManagedError> result() const {
    return receipt_ ? *receipt_ : std::expected<PropertyUpdate<T>, ManagedError>(failure(TreeError::INVALID_ARGUMENT));
  }

private:
  friend class Model<T>;
  friend struct detail::ValueModelAccess;

  ModelField(Model<T> root, Member T::*member, std::source_location origin)
      : root_(std::move(root)), member_(member), origin_(origin) {
  }

  std::unexpected<ManagedError> failure(TreeError cause) const {
    return std::unexpected(
      ManagedError{ManagedPhase::STATE, root_.data_ ? root_.data_->scene : NodeId{}, cause, {}, {}});
  }

  Model<T> root_;
  Member T::*member_;
  std::source_location origin_;
  std::optional<std::expected<PropertyUpdate<T>, ManagedError>> receipt_;
};

template <class T>
template <class Member, class Record>
  requires std::same_as<Record, T> && std::equality_comparable<T> && std::copy_constructible<T> &&
           std::copy_constructible<std::remove_cv_t<Member>> && (!std::is_array_v<Member>) &&
           (!std::is_pointer_v<Member>) && (!portals::detail::IS_BORROWED_DERIVED_VALUE<std::remove_cv_t<Member>>) &&
           (!std::ranges::view<std::remove_cv_t<Member>>)
ModelField<T, Member> Model<T>::field(Member Record::*member, std::source_location origin) const {
  return ModelField<T, Member>(*this, member, origin);
}

/** @brief Wrap an existing writable scalar without copying its value or allocating another Property.
 * @tparam T Equality-comparable owning value accepted by the existing ManagedState.
 * @param state Weak original handle; its scene, origin, lifetime, revision and affinity are preserved.
 * @return Cold writable alias; a foreign scene is rejected when mounted, and a closed source stays closed.
 */
template <std::equality_comparable T> Model<T> model(ManagedState<T> state) {
  return detail::ValueModelAccess::alias(std::move(state), true);
}

/** @brief Wrap an existing read-only scalar without acquiring write authority.
 * @tparam T Equality-comparable owning value accepted by the existing ManagedSource.
 * @param source Weak qualified source; no value snapshot or additional Property is created.
 * @return Cold read-only alias; value()/field() writes return INVALID_ARGUMENT after access checks.
 */
template <std::equality_comparable T> Model<T> model(ManagedSource<T> source) {
  return detail::ValueModelAccess::alias(detail::Access::valueState(source), false);
}

template <class T> Model<NavigationPosition> Navigation<T>::current() const {
  return detail::ValueModelAccess::current(*this);
}

/** @brief Declare a cold scalar, copying its owning initial value without registering a scene.
 * @tparam T Copyable equality-comparable scalar; strings are values and other ranges use collection factories.
 * @param initial Value copied immediately; reachable storage must satisfy Property's owning-value contract.
 * @return Model descriptor sharing one single-scene registration; allocation/copy errors are deferred to mount.
 * Copies and descriptor assignment copy the declaration handle, never publish values.
 */
template <class T>
  requires std::equality_comparable<std::remove_cvref_t<T>> && std::copy_constructible<std::remove_cvref_t<T>> &&
           (!std::ranges::range<std::remove_cvref_t<T>> || std::same_as<std::remove_cvref_t<T>, std::string>) &&
           (!std::is_pointer_v<std::remove_cvref_t<T>>) && (!detail::IS_PROVIDER_MODEL_INPUT<std::remove_cvref_t<T>>)
Model<std::remove_cvref_t<T>> model(T &&initial) {
  return detail::ValueModelAccess::make<std::remove_cvref_t<T>>(initial);
}

/** @brief Copy a text literal as one owning string value, never as a collection of characters.
 * @tparam N Array extent, including the terminator for a text literal.
 * @param initial Bytes copied now; a final terminator is excluded and text is validated by a receiving control.
 * @return Cold scalar string model, including a deferred allocation failure.
 */
template <std::size_t N> Model<std::string> model(const char (&initial)[N]) {
  try {
    const auto count = initial[N - 1] == '\0' ? N - 1 : N;

    return detail::ValueModelAccess::make(std::string(initial, count));
  } catch (const std::bad_alloc &) {
    return detail::ModelAccess::failed<std::string>(TreeError::NO_MEMORY);
  }
}

template <class T>
template <class Value>
  requires(std::same_as<T, Label> || std::same_as<T, Button> || std::same_as<T, Border>) &&
          std::equality_comparable<Value> && std::copy_constructible<Value>
std::expected<void, ManagedError> Ref<T>::setModel(Model<Value> source, std::source_location origin) const {
  detail::SetupFailureScope mask({}, nullptr, nullptr);
  const auto observation = target();
  auto owner = detail::eventScene(observation);
  if (!owner) {
    return std::unexpected(owner.error());
  }

  const auto node = this->node();
  try {
    auto result = detail::ValueModelAccess::replaceCaption(**owner, node, source);
    if (!result) {
      result.error().registrationOrigin = origin;
    }

    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::NO_MEMORY, {}, {}});
  } catch (...) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::CALLBACK_FAILED, {}, {}});
  }
}
} // namespace portals::ui
