// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/collection.hpp>
#include <portals/detail/ui_model_sequence.hpp>
#include <portals/text_format.hpp>
#include <portals/ui_description.hpp>

#include <array>

namespace portals::ui {
template <class T> class Model;
template <class T> class Navigation;
template <class T> class ModelValue;
template <class T, class Member> class ModelField;

namespace detail {
/// @cond INTERNAL
struct ModelAccess;
struct ValueModelAccess;
struct EditAccess;

inline void captureRowDeclarationOrigin(RefTarget &observation) {
  const auto *scope = RowDeclarationOriginScope::current;
  if (scope && scope->origin) {
    // Prepublication observation fields carry only a weak construction owner.
    observation.node = scope->origin->node;
    observation.lifetime = scope->origin;
  }
}

struct ModelDeclarationBase {
  enum class Kind {
    COLLECTION,
    VALUE,
    VALUE_ALIAS,
    VALUE_SOURCE,
    VALUE_DEFERRED_SOURCE
  };

  explicit ModelDeclarationBase(Kind value) : observation(std::make_shared<RefTarget>()), kind(value) {
    captureRowDeclarationOrigin(*observation);
  }

  std::shared_ptr<RefTarget> observation;
  NodeId scene;
  const MountContext *attempt{};
  bool published{};
  bool resolving{};
  const Kind kind;
};

struct ModelDeclarationOriginScope {
  ModelDeclarationOriginScope(ManagedScene &owner, const RefTarget &observation)
      : scene(owner), lifetime(Access::lifetime(owner)) {
    if (observation.node != Node<Widget>{}) {
      auto retained = observation.lifetime.lock();
      if (!retained) {
        failure = TreeError::CLOSED;
        return;
      }
      auto origin = std::const_pointer_cast<portals::detail::ManagedOrigin>(
        std::static_pointer_cast<const portals::detail::ManagedOrigin>(retained));
      auto valid = Access::validateModelOrigin(scene, origin);
      if (!valid) {
        failure = valid.error();
        return;
      }
      declarations.origin = std::move(origin);
    }
    previous = Access::exchangeModelOrigin(scene, declarations.origin);
    shifted = true;
  }

  ~ModelDeclarationOriginScope() {
    if (shifted && !lifetime.expired()) {
      (void)Access::exchangeModelOrigin(scene, std::move(previous));
    }
  }

  ManagedScene &scene;
  std::weak_ptr<const void> lifetime;
  RowDeclarationOriginScope declarations;
  std::shared_ptr<portals::detail::ManagedOrigin> previous;
  std::optional<TreeError> failure;
  bool shifted{};
};

template <class T> struct ModelDeclaration : ModelDeclarationBase {
  using Factory = std::function<std::expected<std::shared_ptr<const ModelSnapshot<T>>, TreeError>()>;

  ModelDeclaration() : ModelDeclarationBase(Kind::COLLECTION) {
  }

  Factory create;
  ModelOptions options;
  ManagedViewHandler<T> prepared;
  ManagedModel<T> handle;
  std::shared_ptr<OwnedSequence<T>> sequence;
};

template <class T> struct NavigationDeclaration {
  explicit NavigationDeclaration(Model<T> value)
      : initial(std::move(value)), observation(std::make_shared<RefTarget>()) {
    captureRowDeclarationOrigin(*observation);
  }

  Model<T> initial;
  NavigationBindingOptions binding;
  NavigationOptions options;
  std::shared_ptr<RefTarget> observation;
  ManagedNavigation handle;
  NodeId scene;
  const MountContext *attempt{};
  bool published{};
  bool resolving{};
};

/// @endcond
} // namespace detail

/** @brief Owning cold model declaration; copies share one single-scene registration.
 * Construction does not read providers or register runtime objects. Closed declarations
 * remain closed; use a new declaration for another scene. Captured borrows retain their
 * original lifetime obligations. Access and registration are creating-UI-thread operations.
 * @tparam T Domain value or collection row; scalar and owned-array paths require copyable values.
 */
template <class T> class Model {
public:
  using Row = T; ///< Immutable domain row, shared with the mounted model.
  /** @brief Create an empty descriptor whose checked use reports INVALID_ARGUMENT. */
  Model() = default;

  /** @brief Obtain the published model without extending scene lifetime.
   * @return Weak runtime handle, CLOSED before publication/after close, or a typed handle/thread failure.
   */
  std::expected<ManagedModel<T>, ManagedError> managed() const {
    if (!data_ || data_->kind != detail::ModelDeclarationBase::Kind::COLLECTION) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, {}, failure_, {}, {}});
    }
    if (data_->observation->owner != std::this_thread::get_id()) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::WRONG_THREAD, {}, {}});
    }
    if (!data_->published) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::CLOSED, {}, {}});
    }
    auto owner = detail::eventScene(data_->observation);
    if (!owner) {
      return std::unexpected(owner.error());
    }
    if (detail::Access::modelSceneClosed(**owner)) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::CLOSED, {}, {}});
    }
    return std::static_pointer_cast<detail::ModelDeclaration<T>>(data_)->handle;
  }

  /** @brief Observe this scalar value; descriptor assignment itself only copies handles.
   * @param origin Assignment diagnostic site retained by the returned proxy.
   * @return UI-affine smart property with checked reads/writes and collected assignment.
   * Collection declarations report INVALID_ARGUMENT; cold or closed values report CLOSED.
   */
  ModelValue<T> value(std::source_location origin = std::source_location::current()) const
    requires std::equality_comparable<T>;

  /** @brief Observe one owned scalar field without borrowing a snapshot or registering another source.
   * @tparam Member Copyable owning field value; const fields expose checked reads only. Known borrowed
   * pointers, string views, spans, reference wrappers and range views are rejected.
   * @tparam Record Declaring record type, required to match this model's value type.
   * @param member Non-null data-member pointer retained by value; null reports INVALID_ARGUMENT on use.
   * @param origin Assignment diagnostic site retained by the returned proxy.
   * @return UI-affine field property sharing this declaration and its root revision/transaction.
   * Cold, collection, wrong-thread and closed failures follow value().
   */
  template <class Member, class Record>
    requires std::same_as<Record, T> && std::equality_comparable<T> && std::copy_constructible<T> &&
             std::copy_constructible<std::remove_cv_t<Member>> && (!std::is_array_v<Member>) &&
             (!std::is_pointer_v<Member>) && (!portals::detail::IS_BORROWED_DERIVED_VALUE<std::remove_cv_t<Member>>) &&
             (!std::ranges::view<std::remove_cv_t<Member>>)
  ModelField<T, Member> field(Member Record::*member,
                              std::source_location origin = std::source_location::current()) const;

  /** @brief Insert one value into a mounted owned sequence on its UI thread.
   * @param index Insertion ordinal, including the end boundary.
   * @param value Value copied under the existing mutation admission boundary.
   * @return Success with a fresh SDK identity, or typed capability, lifetime, capacity or allocation failure.
   * Existing identities survive; failure preserves the installed snapshot.
   */
  std::expected<void, ManagedError> insert(std::uint64_t index, const T &value) const;
  /** @brief Replace one owned row value while preserving its model-local identity and order.
   * @param id Exact live key from this model; missing or stale generations report INVALID_ARGUMENT.
   * @param value Copy-constructible replacement, copied before publication; no assignment is required.
   * @return Success or typed capability, lifetime, reentry, key, revision or allocation failure.
   * Equality-comparable values that compare equal are a no-op; other values publish a revision.
   * Precommit failure preserves the snapshot. Later delivery errors do not roll back an accepted update.
   */
  std::expected<void, ManagedError> update(RowId id, const T &value) const;
  /** @brief Replace an owned row value and collect a checked failure in the existing event diagnostics.
   * @param id Exact model-local row key, with the same validation as update().
   * @param value Replacement copied under update() admission.
   * @param origin Caller location retained by collected diagnostics.
   * @return True on acceptance or equality no-op; false after reporting the update() failure.
   */
  bool row(RowId id, const T &value, std::source_location origin = std::source_location::current()) const;
  /** @brief Erase a range from a mounted owned sequence on its UI thread.
   * @param first Range start; the end boundary is valid for an empty range.
   * @param count Erased count, defaulting to one; zero is a no-op.
   * @return Success or typed capability, lifetime, bounds or allocation failure; rejection preserves the snapshot.
   */
  std::expected<void, ManagedError> erase(std::uint64_t first, std::uint64_t count = 1) const;
  /** @brief Move an owned range without replacing its row identities.
   * @param first Moved range start.
   * @param count Moved row count; zero is a no-op.
   * @param destination Insertion index after removing the range.
   * @return Success or typed capability, lifetime, bounds or allocation failure; rejection preserves the snapshot.
   */
  std::expected<void, ManagedError> move(std::uint64_t first, std::uint64_t count, std::uint64_t destination) const;
  /** @brief Remove every row from a mounted owned sequence on its UI thread.
   * @return Success or typed capability, lifetime or allocation failure; rejection preserves the snapshot.
   * An already empty sequence does not advance its revision.
   */
  std::expected<void, ManagedError> clear() const;

private:
  friend struct detail::ModelAccess;
  friend struct detail::EditAccess;
  friend struct detail::ValueModelAccess;
  template <class> friend class ModelValue;
  template <class, class> friend class ModelField;
  std::shared_ptr<detail::ModelDeclarationBase> data_;
  TreeError failure_{TreeError::INVALID_ARGUMENT};

  struct Replacement {
    RowId id;
    const T &value;
    std::optional<std::uint64_t> expectedVersion;
    std::uint64_t *acceptedVersion{};
  };

  template <class Build>
  std::expected<void, ManagedError> mutate(PositionChange change, bool inserts, Build build,
                                           const Replacement *replacement = nullptr) const;
};

/** @brief Cold explicit shared navigation; ordinary model-source views stay independent.
 * @tparam T Common domain row of the context's model and participating views.
 */
template <class T> class Navigation {
public:
  using Row = T; ///< Common domain row of the explicit navigation session.
  /** @brief Create an empty context descriptor whose checked use reports INVALID_ARGUMENT. */
  Navigation() = default;

  /** @brief Obtain the published context without extending scene lifetime.
   * @return Weak navigation handle, CLOSED before publication/after close, or a typed handle/thread failure.
   */
  std::expected<ManagedNavigation, ManagedError> managed() const {
    if (!data_) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, {}, failure_, {}, {}});
    }
    auto owner = detail::eventScene(data_->observation);
    if (!owner) {
      return std::unexpected(owner.error());
    }
    if (detail::Access::modelSceneClosed(**owner)) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::CLOSED, {}, {}});
    }
    return data_->handle;
  }

  /** @brief Read the current model-local item key from the existing managed navigation channel.
   * @return Logical current RowId, successful nullopt for no current item, or the original
   * cold, closed, wrong-thread or channel access failure. No alias is registered or rows read.
   * The key does not promise materialized data; update() validates its current existence independently.
   */
  std::expected<std::optional<RowId>, ManagedError> currentRow() const {
    auto context = managed();

    if (!context) {
      return std::unexpected(context.error());
    }

    auto fail = [&](TreeError error) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, data_->observation->node.id(), error, {}, {}});
    };
    auto channel = context->current();

    if (!channel) {
      return fail(channel.error());
    }

    auto value = channel->snapshot();

    if (!value) {
      return fail(value.error());
    }

    return (*value)->value.row;
  }

  /** @brief Observe the accepted navigation channel as a cold read-only scalar model.
   * @return Descriptor resolved with this navigation in the receiving mount transaction.
   * No mirror value or additional Property is created; writes report INVALID_ARGUMENT.
   */
  Model<NavigationPosition> current() const;

  /** @brief Admit one model change for the complete shared group on its UI thread.
   * @param target Cold destination source in this scene.
   * @param options Existing remember/reset policy; default same-model use preserves navigation.
   * @return Success or typed admission failure; failed admission retains the installed group.
   */
  std::expected<void, ManagedError> setModel(Model<T> target, ModelRebindOptions options = {}) const;

  /** @brief Change the shared model and report a checked failure through existing scene diagnostics.
   * @param target Cold destination source in this scene.
   * @param options Existing remember/reset policy.
   * @param origin Original command site attached to a reported failure.
   * @return True on success; false after reporting the original checked error.
   */
  bool model(Model<T> target, ModelRebindOptions options = {},
             std::source_location origin = std::source_location::current()) const {
    const auto observation = data_ ? data_->observation : nullptr;
    auto result = setModel(std::move(target), options);
    if (!result) {
      detail::reportEventError(observation, result.error(), origin);
    }
    return static_cast<bool>(result);
  }

private:
  friend struct detail::ModelAccess;
  friend struct detail::EditAccess;
  std::shared_ptr<detail::NavigationDeclaration<T>> data_;
  TreeError failure_{TreeError::INVALID_ARGUMENT};
};

namespace detail {
/// @cond INTERNAL
struct ModelAccess {
  template <class T> static NavigationBindingOptions bindings(const Navigation<T> &value) {
    return value.data_ ? value.data_->binding : NavigationBindingOptions{};
  }

  struct RegisteredSource {
    NodeId scene;
    ModelResource resource;
  };

  // The pure projection may run before descriptor publication in the same mount transaction.
  // Inspect only an already registered handle; never resolve/register a cold catalog entry here.
  template <class T>
  static std::expected<std::optional<RegisteredSource>, TreeError> registeredSource(const Model<T> &model) {
    const auto base = model.data_;
    if (!base) {
      return std::unexpected(model.failure_);
    }
    if (base->kind != ModelDeclarationBase::Kind::COLLECTION) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    if (base->observation->owner != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }
    if (base->scene == NodeId{}) {
      return std::optional<RegisteredSource>{};
    }

    const auto data = std::static_pointer_cast<ModelDeclaration<T>>(base);
    auto resource = Access::registeredModelResource(data->handle);
    if (!resource) {
      return std::unexpected(resource.error());
    }

    return std::optional(RegisteredSource{base->scene, *resource});
  }

  template <class T> static std::shared_ptr<void> identity(const Model<T> &model) {
    return model.data_;
  }

  template <class T>
  static std::expected<std::optional<ManagedModel<T>>, ManagedError> existing(ManagedScene &scene,
                                                                              const Model<T> &value) {
    auto base = value.data_;
    auto data = base && base->kind == ModelDeclarationBase::Kind::COLLECTION
                  ? std::static_pointer_cast<ModelDeclaration<T>>(base)
                  : nullptr;
    const auto fail = [&](TreeError error) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, scene.root().id(), error, {}, {}});
    };
    if (Access::modelSceneClosed(scene)) {
      return fail(TreeError::CLOSED);
    }
    if (!data) {
      return fail(value.failure_);
    }
    if (data->observation->owner != std::this_thread::get_id()) {
      return fail(TreeError::WRONG_THREAD);
    }
    if (data->scene != NodeId{} && data->scene != scene.root().id()) {
      return fail(TreeError::WRONG_SESSION);
    }
    if (data->resolving || (!data->published && data->attempt)) {
      return fail(TreeError::WOULD_BLOCK);
    }
    if (!data->published) {
      return std::optional<ManagedModel<T>>{};
    }
    auto handle = value.managed();
    return handle ? std::expected<std::optional<ManagedModel<T>>, ManagedError>(std::optional(*handle))
                  : std::unexpected(handle.error());
  }

  template <class W> static void keep(ManagedScene &scene, Node<W> node, MountContext &context) {
    try {
      context.rollback.emplace_back([&scene, node, lifetime = Access::lifetime(scene)] {
        if (!lifetime.expired()) {
          (void)scene.remove(node);
        }
      });
    } catch (...) {
      (void)scene.remove(node);
      throw;
    }
  }

  template <class T> static Model<T> make(std::shared_ptr<ModelDeclaration<T>> value) {
    Model<T> result;
    result.data_ = std::move(value);
    return result;
  }

  template <class T> static Model<T> failed(TreeError error) {
    Model<T> result;
    result.failure_ = error;
    return result;
  }

  template <class T> static Navigation<T> make(std::shared_ptr<NavigationDeclaration<T>> value) {
    Navigation<T> result;
    result.data_ = std::move(value);
    return result;
  }

  template <class T> static Navigation<T> failedNavigation(TreeError error) {
    Navigation<T> result;
    result.failure_ = error;
    return result;
  }

  static void publish(MountContext &context) {
    context.committed = true;
    for (auto &action : context.publish) {
      action();
    }
  }

  template <class T>
  static std::expected<ManagedModel<T>, ManagedError> resolve(ManagedScene &scene, const Model<T> &value,
                                                              MountContext &context) {
    auto base = value.data_;
    auto data = base && base->kind == ModelDeclarationBase::Kind::COLLECTION
                  ? std::static_pointer_cast<ModelDeclaration<T>>(base)
                  : nullptr;
    const auto root = scene.root();
    const auto lifetime = Access::lifetime(scene);
    const auto fail = [&](TreeError error) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, root.id(), error, {}, {}});
    };
    if (Access::modelSceneClosed(scene)) {
      return fail(TreeError::CLOSED);
    }
    if (!data) {
      return fail(value.failure_);
    }
    if (data->observation->owner != std::this_thread::get_id()) {
      return fail(TreeError::WRONG_THREAD);
    }
    if (data->resolving) {
      return fail(TreeError::WOULD_BLOCK);
    }
    if (data->scene != NodeId{}) {
      if (data->scene != scene.root().id()) {
        return fail(TreeError::WRONG_SESSION);
      }
      if (data->published) {
        return value.managed();
      }
      return data->attempt == &context ? std::expected<ManagedModel<T>, ManagedError>(data->handle)
                                       : std::expected<ManagedModel<T>, ManagedError>(fail(TreeError::WOULD_BLOCK));
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
      auto initial = data->create();
      if (!initial) {
        return fail(initial.error());
      }
      if (lifetime.expired()) {
        return fail(TreeError::CLOSED);
      }
      // User callable copies are callouts too. Finish that copy and validate
      // lifetime before entering a method through the borrowed scene pointer.
      auto prepared = data->prepared;
      if (lifetime.expired()) {
        return fail(TreeError::CLOSED);
      }
      auto created = scene.model<T>(std::move(*initial), data->options, std::move(prepared));
      if (!created) {
        return std::unexpected(created.error());
      }
      if (lifetime.expired()) {
        return fail(TreeError::CLOSED);
      }
      Access::editDeclaration(*created, data);
      if (data->sequence) {
        Access::ownedModel(*created);
      }
      bool rollbackInstalled{};
      try {
        context.rollback.emplace_back([&scene, data, lifetime, handle = *created] {
          if (!lifetime.expired()) {
            auto copy = handle;
            Access::retireChoiceModel(scene, copy);
          }
          data->handle = {};
          data->scene = {};
          data->attempt = nullptr;
        });
        rollbackInstalled = true;
        context.publish.emplace_back([&scene, data, root, lifetime] {
          data->observation->scene = &scene;
          data->observation->lifetime = lifetime;
          data->observation->node = root;
          data->attempt = nullptr;
          data->published = true;
        });
      } catch (...) {
        if (!rollbackInstalled) {
          auto copy = *created;
          Access::retireChoiceModel(scene, copy);
        }
        throw;
      }
      data->handle = *created;
      data->scene = scene.root().id();
      data->attempt = &context;
      return *created;
    } catch (const std::bad_alloc &) {
      return fail(lifetime.expired() ? TreeError::CLOSED : TreeError::NO_MEMORY);
    } catch (...) {
      return fail(lifetime.expired() ? TreeError::CLOSED : TreeError::CALLBACK_FAILED);
    }
  }

  template <class T>
  static std::expected<ManagedNavigation, ManagedError> resolve(ManagedScene &scene, const Navigation<T> &value,
                                                                MountContext &context) {
    auto data = value.data_;
    const auto root = scene.root();
    const auto lifetime = Access::lifetime(scene);
    const auto fail = [&](TreeError error) {
      return std::unexpected(ManagedError{ManagedPhase::STATE, root.id(), error, {}, {}});
    };
    if (Access::modelSceneClosed(scene)) {
      return fail(TreeError::CLOSED);
    }
    if (!data) {
      return fail(value.failure_);
    }
    if (data->observation->owner != std::this_thread::get_id()) {
      return fail(TreeError::WRONG_THREAD);
    }
    if (data->resolving) {
      return fail(TreeError::WOULD_BLOCK);
    }
    if (data->scene != NodeId{}) {
      if (data->scene != scene.root().id()) {
        return fail(TreeError::WRONG_SESSION);
      }
      if (data->published) {
        return value.managed();
      }
      return data->attempt == &context ? std::expected<ManagedNavigation, ManagedError>(data->handle)
                                       : std::expected<ManagedNavigation, ManagedError>(fail(TreeError::WOULD_BLOCK));
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
    auto model = resolve(scene, data->initial, context);
    if (!model) {
      return std::unexpected(model.error());
    }
    auto created = scene.navigation(*model, data->options);
    if (!created) {
      return std::unexpected(created.error());
    }
    try {
      context.rollback.emplace_back([&scene, data, lifetime, handle = *created] {
        if (!lifetime.expired()) {
          Access::retireColdNavigation(scene, handle);
        }
        data->handle = {};
        data->scene = {};
        data->attempt = nullptr;
      });
    } catch (const std::bad_alloc &) {
      Access::retireColdNavigation(scene, *created);
      return fail(TreeError::NO_MEMORY);
    } catch (...) {
      Access::retireColdNavigation(scene, *created);
      return fail(TreeError::CALLBACK_FAILED);
    }
    try {
      context.publish.emplace_back([&scene, data, root, lifetime] {
        data->observation->scene = &scene;
        data->observation->lifetime = lifetime;
        data->observation->node = root;
        data->attempt = nullptr;
        data->published = true;
      });
    } catch (const std::bad_alloc &) {
      return fail(TreeError::NO_MEMORY);
    } catch (...) {
      return fail(TreeError::CALLBACK_FAILED);
    }
    data->handle = *created;
    data->scene = scene.root().id();
    data->attempt = &context;
    return *created;
  }

  template <class T>
  static std::expected<ManagedModel<T>, ManagedError> source(ManagedScene &scene, const Model<T> &value,
                                                             MountContext &context) {
    return resolve(scene, value, context);
  }

  template <class T>
  static std::expected<ManagedModel<T>, ManagedError> source(ManagedScene &scene, const Navigation<T> &value,
                                                             MountContext &context) {
    auto navigation = resolve(scene, value, context);
    return navigation ? Access::navigationModel<T>(scene, *navigation) : std::unexpected(navigation.error());
  }

  template <class T, class W>
  static std::expected<void, ManagedError> bind(ManagedScene &, Node<W>, const Model<T> &, MountContext &) {
    return {};
  }

  template <class T, class W>
  static std::expected<void, ManagedError> bind(ManagedScene &scene, Node<W> node, const Navigation<T> &value,
                                                MountContext &context) {
    auto navigation = resolve(scene, value, context);
    return navigation ? scene.bindNavigation(node, *navigation, value.data_->binding)
                      : std::unexpected(navigation.error());
  }
};

/// @endcond
} // namespace detail

/** @brief Declare a provider without reading, copying rows or registering it before mount.
 * @tparam T Immutable provider row; copying or default construction is not required.
 * @param provider Shared immutable source; no model preparation or provider callback runs here.
 * @param options Existing finite model budgets.
 * @param prepared Model-owned preparation hook used unless a view explicitly overrides it.
 * @return Cold source, including a factory allocation failure reported by later checked admission.
 */
template <class T>
Model<T> model(std::shared_ptr<const ModelSnapshot<T>> provider, ModelOptions options = {},
               ManagedViewHandler<T> prepared = {}) {
  try {
    auto data = std::make_shared<detail::ModelDeclaration<T>>();
    data->options = options;
    data->prepared = std::move(prepared);
    data->create = [provider = std::move(provider)] {
      return std::expected<std::shared_ptr<const ModelSnapshot<T>>, TreeError>(provider);
    };
    return detail::ModelAccess::make(std::move(data));
  } catch (const std::bad_alloc &) {
    return detail::ModelAccess::failed<T>(TreeError::NO_MEMORY);
  } catch (...) {
    return detail::ModelAccess::failed<T>(TreeError::CALLBACK_FAILED);
  }
}

/** @brief Declare a bounded owning sequence with SDK-assigned stable identities.
 * @tparam T Copyable domain value; contained references retain their normal borrow semantics.
 * @tparam N Initial number of values, checked against the configured capacity at mount.
 * @param values Values owned by the cold declaration; captured borrows remain caller obligations.
 * @param limits Whole-sequence capacity, defaulting to 4096 rows.
 * @param options Existing finite model budgets.
 * @param history Rolling scalar-proof retention, defaulting to the latest 1024 structural changes.
 * @return Cold model; registration allocates identities under the mounted error boundary.
 * Edits continue after proof expiry. Live identities remain exact; old missing identities report UNAVAILABLE.
 */
template <class T, std::size_t N>
Model<T> model(std::array<T, N> values, CollectionLimits limits = {}, ModelOptions options = {},
               PositionMapOptions history = {}) {
  try {
    auto data = std::make_shared<detail::ModelDeclaration<T>>();
    data->options = options;
    auto sequence = std::make_shared<detail::OwnedSequence<T>>();
    sequence->capacity = limits.maxRows;
    sequence->history = history;
    data->sequence = sequence;
    data->create = [values = std::move(values), limits,
                    sequence]() -> std::expected<std::shared_ptr<const ModelSnapshot<T>>, TreeError> {
      if (!limits.maxRows || !sequence->history.maxChanges || N > limits.maxRows) {
        return std::unexpected(!limits.maxRows || !sequence->history.maxChanges ? TreeError::INVALID_ARGUMENT
                                                                                : TreeError::NODE_LIMIT);
      }
      if (sequence->current) {
        return sequence->current;
      }

      std::vector<ModelRow<T>> rows(std::initializer_list<ModelRow<T>>{});
      rows.reserve(N);
      for (std::size_t index = 0; index < N; ++index) {
        rows.push_back({{index + 1, 1}, values[index]});
      }
      auto proof = std::make_shared<detail::OwnedHistory>(1, N, sequence->history);
      sequence->current = std::make_shared<detail::OwnedRows<T>>(rows, 1, std::move(proof));
      sequence->nextId = N + 1;
      return sequence->current;
    };
    return detail::ModelAccess::make(std::move(data));
  } catch (const std::bad_alloc &) {
    return detail::ModelAccess::failed<T>(TreeError::NO_MEMORY);
  } catch (...) {
    return detail::ModelAccess::failed<T>(TreeError::CALLBACK_FAILED);
  }
}

/** @brief Declare one explicit shared navigation session; no Property is created yet.
 * @tparam T Common domain row of the model and consuming views.
 * @param source Cold initial model.
 * @param binding Channels shared by consuming views; reading-anchor sharing is optional.
 * @param options Existing navigation and checkpoint admission limits.
 * @return Cold context; copies share this declaration, while separate calls create independent sessions.
 */
template <class T>
Navigation<T> navigation(Model<T> source, NavigationBindingOptions binding = {}, NavigationOptions options = {}) {
  try {
    auto data = std::make_shared<detail::NavigationDeclaration<T>>(std::move(source));
    data->binding = binding;
    data->options = options;
    return detail::ModelAccess::make(std::move(data));
  } catch (const std::bad_alloc &) {
    return detail::ModelAccess::failedNavigation<T>(TreeError::NO_MEMORY);
  } catch (...) {
    return detail::ModelAccess::failedNavigation<T>(TreeError::CALLBACK_FAILED);
  }
}

template <class T>
std::expected<void, ManagedError> Navigation<T>::setModel(Model<T> target, ModelRebindOptions options) const {
  const auto data = data_;
  auto owner = data ? detail::eventScene(data->observation)
                    : std::expected<ManagedScene *, ManagedError>(
                        std::unexpected(ManagedError{ManagedPhase::INPUT, {}, failure_, {}, {}}));
  if (!owner) {
    return std::unexpected(owner.error());
  }
  auto existing = detail::ModelAccess::existing(**owner, target);
  if (!existing) {
    return std::unexpected(existing.error());
  }
  if (*existing) {
    auto result = data->handle.setModel(**existing, options);
    if (result) {
      data->initial = target;
    }
    return result;
  }
  try {
    detail::MountContext context;
    auto model = detail::ModelAccess::resolve(**owner, target, context);
    if (!model) {
      return std::unexpected(model.error());
    }
    auto changed = data->handle.setModel(*model, options);
    if (!changed) {
      return changed;
    }
    detail::ModelAccess::publish(context);
    data->initial = target;
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(ManagedError{ManagedPhase::INPUT, {}, TreeError::NO_MEMORY, {}, {}});
  } catch (...) {
    return std::unexpected(ManagedError{ManagedPhase::INPUT, {}, TreeError::CALLBACK_FAILED, {}, {}});
  }
}

template <class T>
template <class Build>
std::expected<void, ManagedError> Model<T>::mutate(PositionChange change, bool inserts, Build build,
                                                   const Replacement *replacement) const {
  auto handle = managed();
  if (!handle) {
    return std::unexpected(handle.error());
  }
  auto owner = detail::eventScene(data_->observation);
  if (!owner) {
    return std::unexpected(owner.error());
  }
  const auto root = data_->observation->node;
  const auto lifetime = data_->observation->lifetime;
  auto fail = [&](TreeError error) {
    return std::unexpected(ManagedError{ManagedPhase::STATE, root.id(), error, {}, {}});
  };
  if constexpr (!std::is_copy_constructible_v<T>) {
    return fail(TreeError::INVALID_ARGUMENT);
  } else {
    auto sequence = std::static_pointer_cast<detail::ModelDeclaration<T>>(data_)->sequence;
    if (!sequence || !sequence->current) {
      return fail(TreeError::INVALID_ARGUMENT);
    }
    if (sequence->editing || data_->resolving) {
      return fail(TreeError::WOULD_BLOCK);
    }
    const auto previous = sequence->current;
    const auto count = previous->rowCount();
    if (change.first > count || change.removed > count - change.first ||
        (change.kind == PositionChangeKind::MOVE && change.destination > count - change.removed)) {
      return fail(TreeError::INVALID_ARGUMENT);
    }
    if (!replacement && ((!change.removed && !change.inserted) ||
                         (change.kind == PositionChangeKind::MOVE && change.destination == change.first))) {
      return {};
    }
    if (change.inserted > std::numeric_limits<std::uint64_t>::max() - (count - change.removed) ||
        count - change.removed + change.inserted > sequence->capacity) {
      return fail(TreeError::NODE_LIMIT);
    }

    struct Editing {
      detail::OwnedSequence<T> &sequence;
      bool active{true};

      ~Editing() {
        if (active) {
          sequence.editing = false;
        }
      }
    } guard{*sequence};

    sequence->editing = true;
    std::shared_ptr<detail::OwnedEditVersion> editVersion;
    if (replacement && sequence->editVersions) {
      for (const auto &weak : *sequence->editVersions) {
        auto entry = weak.lock();
        if (entry && entry->row == replacement->id) {
          editVersion = std::move(entry);
          break;
        }
      }
    }
    if (replacement && replacement->expectedVersion &&
        (!editVersion || editVersion->version != *replacement->expectedVersion)) {
      return fail(TreeError::WOULD_BLOCK);
    }
    if (editVersion && editVersion->version == std::numeric_limits<std::uint64_t>::max()) {
      return fail(TreeError::IDENTITY_EXHAUSTED);
    }
    try {
      if (replacement) {
        const auto found = previous->locateRow(replacement->id);

        if (found.state != RowLookupState::FOUND) {
          return fail(TreeError::INVALID_ARGUMENT);
        }

        if constexpr (std::equality_comparable<T>) {
          const bool equal = previous->rows()[static_cast<std::size_t>(found.index)].value == replacement->value;

          if (lifetime.expired() || detail::Access::modelSceneClosed(**owner)) {
            return fail(TreeError::CLOSED);
          }

          if (equal) {
            if (replacement->acceptedVersion) {
              *replacement->acceptedVersion = editVersion->version;
            }
            return {};
          }
        }
      }

      if (previous->revision() == std::numeric_limits<std::uint64_t>::max() ||
          (inserts && sequence->nextId == std::numeric_limits<std::uint64_t>::max())) {
        return fail(TreeError::IDENTITY_EXHAUSTED);
      }

      std::vector<ModelRow<T>> rows(std::initializer_list<ModelRow<T>>{});
      const auto nextCount = change.kind == PositionChangeKind::MOVE ? count : count - change.removed + change.inserted;
      rows.reserve(static_cast<std::size_t>(nextCount));
      std::invoke(build, rows, previous->rows(), sequence->nextId);
      auto history = std::make_shared<detail::OwnedHistory>(previous->history(), count,
                                                            replacement ? std::nullopt : std::optional(change));
      auto next = std::make_shared<detail::OwnedRows<T>>(rows, previous->revision() + 1, std::move(history));
      if (lifetime.expired()) {
        return fail(TreeError::CLOSED);
      }
      if (sequence->current != previous) {
        return fail(TreeError::WOULD_BLOCK);
      }
      auto applied = detail::Access::applyOwnedModel(**owner, *handle, std::shared_ptr<const ModelSnapshot<T>>(next));
      if (!applied) {
        return applied;
      }
      // All fallible work precedes the existing model transaction. Publish owner
      // metadata before releasing old rows, whose user destructors may reenter.
      if (editVersion) {
        ++editVersion->version;
      }
      if (replacement && replacement->acceptedVersion) {
        *replacement->acceptedVersion = editVersion->version;
      }
      sequence->current = std::move(next);
      if (inserts) {
        ++sequence->nextId;
      }
      sequence->editing = false;
      guard.active = false;
      return {};
    } catch (const std::bad_alloc &) {
      return fail(lifetime.expired() ? TreeError::CLOSED : TreeError::NO_MEMORY);
    } catch (...) {
      return fail(lifetime.expired() ? TreeError::CLOSED : TreeError::CALLBACK_FAILED);
    }
  }
}

template <class T> std::expected<void, ManagedError> Model<T>::insert(std::uint64_t index, const T &value) const {
  return mutate(PositionChange::splice(index, 0, 1), true, [&](auto &rows, auto old, std::uint64_t identity) {
    for (std::size_t offset = 0; offset <= old.size(); ++offset) {
      if (offset == index) {
        rows.emplace_back(RowId{identity, 1}, value);
      }
      if (offset < old.size()) {
        rows.push_back(old[offset]);
      }
    }
  });
}

template <class T> std::expected<void, ManagedError> Model<T>::update(RowId id, const T &value) const {
  const Replacement replacement{id, value};

  return mutate(
    {}, false,
    [&](auto &rows, auto old, std::uint64_t) {
      for (const auto &row : old) {
        rows.emplace_back(row.id, row.id == id ? value : row.value);
      }
    },
    &replacement);
}

template <class T> bool Model<T>::row(RowId id, const T &value, std::source_location origin) const {
  const auto observation = data_ ? data_->observation : nullptr;
  auto result = update(id, value);

  if (!result) {
    detail::reportEventError(observation, result.error(), origin);
  }

  return result.has_value();
}

template <class T> std::expected<void, ManagedError> Model<T>::erase(std::uint64_t first, std::uint64_t count) const {
  return mutate(PositionChange::splice(first, count, 0), false, [&](auto &rows, auto old, std::uint64_t) {
    for (std::size_t offset = 0; offset < old.size(); ++offset) {
      if (offset < first || offset - first >= count) {
        rows.push_back(old[offset]);
      }
    }
  });
}

template <class T>
std::expected<void, ManagedError> Model<T>::move(std::uint64_t first, std::uint64_t count,
                                                 std::uint64_t destination) const {
  return mutate(PositionChange::move(first, count, destination), false, [&](auto &rows, auto old, std::uint64_t) {
    for (std::size_t offset = 0; offset < old.size(); ++offset) {
      const bool moved = offset >= destination && offset - destination < count;
      std::uint64_t original{};
      if (moved) {
        original = first + offset - destination;
      } else {
        const auto remaining = offset < destination ? offset : offset - count;
        original = remaining < first ? remaining : remaining + count;
      }
      rows.push_back(old[static_cast<std::size_t>(original)]);
    }
  });
}

template <class T> std::expected<void, ManagedError> Model<T>::clear() const {
  auto handle = managed();
  if (!handle) {
    return std::unexpected(handle.error());
  }
  const auto data = std::static_pointer_cast<detail::ModelDeclaration<T>>(data_);
  if (!data->sequence || !data->sequence->current) {
    return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::INVALID_ARGUMENT, {}, {}});
  }
  return erase(0, data->sequence->current->rowCount());
}

template <class T>
template <class Row>
  requires(std::same_as<T, VirtualList<Row>> || std::same_as<T, VirtualTable<Row>>)
std::expected<void, ManagedError> Ref<T>::setModel(Model<Row> source, ModelRebindOptions options,
                                                   std::source_location origin) const {
  const auto observation = target();
  const auto node = this->node();
  auto owner = detail::eventScene(observation);
  if (!owner) {
    auto failure = owner.error();
    failure.registrationOrigin = origin;
    return std::unexpected(failure);
  }
  auto existing = detail::ModelAccess::existing(**owner, source);
  if (!existing) {
    auto failure = existing.error();
    failure.registrationOrigin = origin;
    return std::unexpected(failure);
  }
  if (*existing) {
    auto changed = (*owner)->setModel(node, **existing, std::nullopt, options);
    if (!changed) {
      changed.error().registrationOrigin = origin;
    }
    return changed;
  }
  try {
    detail::MountContext context;
    auto model = detail::ModelAccess::resolve(**owner, source, context);
    if (!model) {
      auto failure = model.error();
      failure.registrationOrigin = origin;
      return std::unexpected(failure);
    }
    auto changed = (*owner)->setModel(node, *model, std::nullopt, options);
    if (!changed) {
      changed.error().registrationOrigin = origin;
      return changed;
    }
    detail::ModelAccess::publish(context);
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(ManagedError{ManagedPhase::INPUT, {}, TreeError::NO_MEMORY, {}, {}});
  } catch (...) {
    return std::unexpected(ManagedError{ManagedPhase::INPUT, {}, TreeError::CALLBACK_FAILED, {}, {}});
  }
}
} // namespace portals::ui

#include <portals/detail/ui_source_projection.hpp>
#include <portals/detail/ui_value_model.hpp>
