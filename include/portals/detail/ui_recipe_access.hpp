// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/recipe_state.hpp>
#include <portals/detail/ui_recipe_context.hpp>
#include <portals/ui_action.hpp>

#include <format>

namespace portals {
struct SearchItem;

namespace detail {
struct FindExternalBinding;
struct EditTextBinding;
} // namespace detail
} // namespace portals

namespace portals::ui {
struct FormFieldConfig;
struct LineEditConfig;
struct FindBarConfig;
struct SuggestionsConfig;

namespace detail {
/** @brief Internal bridge for checked access to managed scene and recipe state. */
struct Access {
  /// @cond INTERNAL
  static Description<VirtualList<SearchItem>>
  suggestions(TextSource source, std::shared_ptr<portals::detail::FindModelSource> model, SuggestionsConfig config);
  static std::expected<TextSource, ManagedError> resolveSearchText(ManagedScene &, const TextSource &, MountContext &);
  static std::expected<bool, ManagedError> readSearchText(ManagedScene &, const TextSource &, std::size_t,
                                                          std::string &, bool &);
  static std::expected<void, ManagedError> configureSearchList(ManagedScene &, Node<VirtualList<SearchItem>>,
                                                               std::shared_ptr<void>, std::function<void(RowId)>,
                                                               CellStyle);
  static std::expected<void, ManagedError> suppressSearchList(ManagedScene &, Node<VirtualList<SearchItem>>, bool);
  static std::expected<void, ManagedError> configureChoiceList(ManagedScene &, Node<VirtualList<SearchItem>>,
                                                               std::shared_ptr<void>, std::function<void(RowId)>);
  static std::expected<std::optional<ModelRow<SearchItem>>, ManagedError>
  choiceRow(ManagedScene &, Node<VirtualList<SearchItem>>, RowId);
  static std::expected<std::optional<RowId>, ManagedError> choiceFirstRow(ManagedScene &,
                                                                          Node<VirtualList<SearchItem>>);
  static std::expected<std::optional<RowId>, ManagedError>
  choiceNavigationRow(ManagedScene &, Node<VirtualList<SearchItem>>, std::uint64_t, bool, std::int64_t);
  static std::expected<std::optional<std::uint64_t>, ManagedError>
  choiceMoveNavigation(ManagedScene &, Node<VirtualList<SearchItem>>, portals::Action, int pageDirection = 0);
  static std::expected<void, ManagedError> choiceClickActivation(ManagedScene &, Node<VirtualList<SearchItem>>);
  static std::expected<std::uint64_t, ManagedError> choiceDraftRevision(ManagedScene &, Node<LineEdit>);
  static std::expected<void, ManagedError> bindChoiceTrigger(ManagedScene &, Node<Button>,
                                                             std::function<std::expected<void, ManagedError>()>);
  static std::expected<Node<OverlayHost>, ManagedError> choiceLayerHost(ManagedScene &, Node<Widget>);
  static std::expected<void, ManagedError> claimChoicePopup(ManagedScene &, std::shared_ptr<SceneActivity>);
  static bool choicePopupOpened(ManagedScene &, Node<Dialog>);

  static void requestChoicePopup(ManagedScene &scene, std::shared_ptr<SceneActivity> activity) {
    scene.choiceIntent_ = std::move(activity);
  }

  static void cancelChoicePopup(ManagedScene &scene, const SceneActivity *activity) {
    if (auto pending = scene.choiceIntent_.lock(); pending.get() == activity) {
      scene.choiceIntent_.reset();
    }
  }

  template <class T> static void retireChoiceModel(ManagedScene &scene, ManagedModel<T> &model) noexcept {
    auto source = model.lock();
    if (!source) {
      return;
    }
    std::erase(scene.models_, *source);
    model = {};
    // Retiring a user preparation callback can destroy the scene. The source
    // is already detached, and its strong local owner needs no scene access.
    (*source)->retire();
  }

  static bool modelSceneClosed(const ManagedScene &scene) noexcept {
    return scene.closed_;
  }

  template <std::equality_comparable T> static ManagedState<T> valueState(const ManagedSource<T> &source) {
    return source.state_;
  }

  template <std::equality_comparable T>
  static std::expected<ManagedState<T>, ManagedError> createValueState(ManagedScene &scene, const T &initial) {
    // Owning scalar records may contain containers whose Debug move allocates inside noexcept.
    return scene.stateFrom<T>(initial, Invalidation::PAINT, {});
  }

  template <std::equality_comparable T>
  static std::expected<void, TreeError> validateValueState(ManagedScene &scene, const ManagedState<T> &state) {
    // lock preserves the original source's affinity, lifetime and origin qualification.
    auto property = state.lock();
    if (!property) {
      return std::unexpected(property.error());
    }
    if (state.scene_ != scene.root_.id()) {
      return std::unexpected(TreeError::WRONG_SESSION);
    }

    return {};
  }

  template <class T> static void editDeclaration(ManagedModel<T> model, std::shared_ptr<void> declaration) {
    if (auto source = model.lock()) {
      (*source)->editDeclaration = std::move(declaration);
    }
  }

  template <class T> static void ownedModel(ManagedModel<T> model) {
    if (auto source = model.lock()) {
      (*source)->sinkOnly = true;
    }
  }

  template <class T>
  static std::expected<void, ManagedError> applyOwnedModel(ManagedScene &scene, ManagedModel<T> model,
                                                           std::shared_ptr<const ModelSnapshot<T>> snapshot) {
    return scene.applyOwnedSnapshot(std::move(model), std::move(snapshot));
  }

  template <class T>
  static std::expected<ManagedModel<T>, ManagedError> navigationModel(ManagedScene &scene,
                                                                      const ManagedNavigation &navigation) {
    auto record = navigation.lock();
    auto source =
      record ? std::dynamic_pointer_cast<portals::detail::ManagedModelSourceOf<T>>((*record)->model.lock()) : nullptr;
    if (!source || source->retired || (*record)->scene != scene.root_.id()) {
      return std::unexpected(ManagedError{
        ManagedPhase::STATE, scene.root_.id(), record ? TreeError::INVALID_ARGUMENT : record.error(), {}, {}});
    }
    ManagedModel<T> result;
    result.source_ = source;
    result.owner_ = scene.owner_;
    return result;
  }

  static void retireColdNavigation(ManagedScene &scene, ManagedNavigation navigation) noexcept {
    auto record = navigation.record_.lock();
    if (!record || !scene.navigation_) {
      return;
    }
    auto current = record->current.model_.lock();
    auto selection = record->selection.model_.lock();
    auto reading = record->reading.model_.lock();
    record->closed = true;
    std::erase(scene.navigation_->contexts, record);
    std::erase_if(scene.properties_, [&](const auto &property) {
      return property == current || property == selection || property == reading;
    });
    record->close();
  }

  template <class T> static void retireChoiceState(ManagedScene &scene, ManagedState<T> &state) noexcept {
    auto property = state.model_.lock();
    if (property) {
      std::erase(scene.properties_, std::shared_ptr<void>(std::move(property)));
    }
    state = {};
  }

  template <std::equality_comparable T>
  static void retireValueState(ManagedScene &scene, ManagedState<T> &state) noexcept {
    auto property = state.model_.lock();

    // Detach every weak declaration handle before a last value destructor can reenter.
    state = {};
    if (property) {
      std::erase(scene.properties_, std::shared_ptr<void>(property));
    }
    // Releasing the pinned property may destroy the scene. Do not access it again.
  }

  static void reportValueError(const std::shared_ptr<RefTarget> &target, ManagedError &failure,
                               std::source_location origin, const char *operation = "model.value",
                               const char *control = "Model") noexcept {
    failure.operation = operation;
    failure.operationOrigin = origin;

    if (target && target->owner == std::this_thread::get_id() && !target->lifetime.expired()) {
      target->scene->reportShortError(failure, operation, {}, control, origin);
    } else {
      const auto ticket = SetupFailureScope::latch(failure);
    }
  }

  template <class T>
  static std::expected<ModelResource, TreeError> registeredModelResource(const ManagedModel<T> &model) {
    auto source = model.lock();
    if (!source || (*source)->retired || !(*source)->model) {
      return std::unexpected(source ? TreeError::CLOSED : source.error());
    }

    return (*source)->navigationResource;
  }

  template <class T> static std::expected<std::string, TreeError> formatValueCaption(const T &value) {
    if constexpr (std::same_as<T, GraphemeText>) {
      return std::expected<std::string, TreeError>(std::in_place, value.bytes());
    } else if constexpr (std::same_as<T, std::string>) {
      return std::expected<std::string, TreeError>(std::in_place, value.data(), value.size());
    } else {
      const auto text = std::format("{}", value);

      // Copy directly into the checked result: a debug string move can allocate inside noexcept.
      return std::expected<std::string, TreeError>(std::in_place, text.data(), text.size());
    }
  }

  // Optional ownership lives only on captions using value sources. Pins keep every erased callback/value
  // alive until all scene containers are consistent; their final destructors may retire the scene.
  struct ValueCaptionGraph {
    ManagedScene *scene{};
    std::weak_ptr<const void> lifetime;
    std::shared_ptr<void> output;
    std::vector<std::shared_ptr<ManagedScene::ObserverRecord>> observers =
      std::vector<std::shared_ptr<ManagedScene::ObserverRecord>>(
        std::initializer_list<std::shared_ptr<ManagedScene::ObserverRecord>>{});
    std::shared_ptr<void> payload;

    ~ValueCaptionGraph() {
      if (lifetime.expired()) {
        return;
      }

      for (const auto &observer : observers) {
        observer->retired = true;
      }

      for (const auto &observer : observers) {
        observer->subscription = {};
        std::erase(scene->observers_, observer);
      }

      if (output) {
        std::erase(scene->properties_, output);
      }
      // No scene access after member destruction starts releasing user captures.
    }
  };

  static std::expected<ManagedState<std::string>, ManagedError>
  valueTextState(ManagedScene &scene, const std::string &initial, const std::shared_ptr<ValueCaptionGraph> &graph) {
    auto state = scene.state(initial);
    if (!state) {
      return std::unexpected(state.error());
    }

    graph->output = *state->lock();

    return *state;
  }

  template <class Compute> static auto computeValueText(ManagedScene &scene, Compute &&compute) {
    struct Computing {
      std::size_t &count;

      ~Computing() {
        --count;
      }
    } guard{scene.observerCallbacks_};

    ++scene.observerCallbacks_;

    return std::invoke(std::forward<Compute>(compute));
  }

  template <std::equality_comparable T>
  static std::expected<void, ManagedError> observeValueText(ManagedScene &scene, Node<Widget> node,
                                                            ManagedState<T> source,
                                                            std::function<std::expected<void, TreeError>()> callback,
                                                            const std::shared_ptr<ValueCaptionGraph> &graph) {
    auto observed = scene.observe(node, source, [callback = std::move(callback)](const T &) {
      return callback();
    });
    if (!observed) {
      return observed;
    }

    const auto record = scene.observers_.back();
    auto snapshot = source.snapshot();
    if (!snapshot) {
      record->retired = true;
      record->subscription = {};
      std::erase(scene.observers_, record);

      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), snapshot.error(), {}, {}});
    }

    record->successful = (*snapshot)->revision;
    record->initialPending = false;
    record->operation = "text.compute";
    graph->observers.push_back(record);

    return {};
  }

  static std::expected<void, ManagedError> admitValueTextObservers(ManagedScene &scene, Node<Widget> node,
                                                                   std::size_t count) {
    auto admitted = scene.observerAdmission(node);
    if (!admitted) {
      return admitted;
    }

    const auto boundModels = std::ranges::count_if(scene.models_, [](const auto &source) {
      return source->bound();
    });
    const auto nodeObservers = std::ranges::count(scene.observers_, Node<Widget>(node), [](const auto &record) {
      return record->node;
    });
    const auto captures = std::ranges::count_if(scene.liveText_, [&](const auto &record) {
      return record->origin->node == node;
    });
    const auto used = scene.observers_.size() + scene.liveText_.size() + static_cast<std::size_t>(boundModels);
    const auto nodeUsed = static_cast<std::size_t>(nodeObservers + captures);

    if (count > scene.options_.maxObservers - used || count > scene.options_.maxObserversPerNode - nodeUsed) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::NODE_LIMIT, {}, {}});
    }

    return {};
  }

  struct ValueCaptionSlot {
    const void *identity{};
    std::shared_ptr<ValueCaptionBinding> source;
  };

  static std::expected<ValueCaptionSlot, ManagedError> valueCaptionSlot(ManagedScene &scene, Node<Widget> node) {
    auto allowed = scene.boundary(ManagedPhase::BIND, node);
    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    auto entry = scene.find(node);
    if (!entry) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }

    return ValueCaptionSlot{entry->caption.get(), entry->caption ? entry->caption->source : nullptr};
  }

  template <class Receiver>
  static std::expected<void, ManagedError>
  installValueCaption(ManagedScene &scene, Node<Receiver> node, ManagedState<std::string> text,
                      std::shared_ptr<ValueCaptionBinding> source, const void *previous, MountContext &context) {
    auto slot = valueCaptionSlot(scene, node);
    if (!slot) {
      return std::unexpected(slot.error());
    }
    if (slot->identity != previous) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::WOULD_BLOCK, {}, {}});
    }

    auto candidate = std::make_unique<ManagedScene::CaptionBinding>();

    if constexpr (std::same_as<Receiver, Border>) {
      candidate->node = node;
    } else {
      candidate->node = Node<Control>(node);
    }
    candidate->model = text;
    candidate->source = std::move(source);

    auto retired = std::make_shared<std::unique_ptr<ManagedScene::CaptionBinding>>();

    context.release.emplace_back([retired] {
    });

    // syncCaption validates and allocates text before setControlText. Publication after that is a swap
    // and has no allocation/callout. A failed candidate therefore cannot leave the old source detached.
    auto synced = scene.syncCaption(node, *candidate, ManagedPhase::BIND);
    if (!synced) {
      return synced;
    }

    auto entry = scene.find(node);
    entry->caption.swap(candidate);
    retired->swap(candidate);

    // The transaction keeps the previous graph until descriptor publication has finished. Its final
    // callable destructor may destroy the scene, so retirement must follow the last scene access.

    return {};
  }

  template <std::equality_comparable T, class Projection>
  static std::expected<ManagedState<std::string>, ManagedError>
  projectValueCaption(ManagedScene &scene, Node<Widget> node, ManagedState<T> source,
                      const std::shared_ptr<Projection> &projection, const std::shared_ptr<ValueCaptionGraph> &graph) {
    auto admitted = scene.observerAdmission(node);
    if (!admitted) {
      return std::unexpected(admitted.error());
    }

    graph->observers.reserve(1);
    // Only shared ownership is copied inside derive. Constructing the user's callable happens before
    // entering it; its pure invocation follows the existing derive non-reentry contract.
    auto text = scene.derive(source, [projection](const T &value) -> std::expected<std::string, TreeError> {
      if constexpr (std::is_member_pointer_v<Projection> || std::is_pointer_v<Projection>) {
        if (!*projection) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }
      }

      decltype(auto) projected = std::invoke(*projection, value);

      if constexpr (portals::detail::IS_DERIVED_EXPECTED<std::remove_cvref_t<decltype(projected)>>) {
        return projected ? formatValueCaption(*projected)
                         : std::expected<std::string, TreeError>(std::unexpected(projected.error()));
      } else {
        return formatValueCaption(projected);
      }
    });
    if (!text) {
      return std::unexpected(text.error());
    }

    graph->output = *text->state_.lock();
    graph->observers.push_back(scene.observers_.back());
    graph->observers.back()->node = node;

    return text->state_;
  }

  static bool choiceWithin(ManagedScene &scene, Node<Widget> root, Node<Widget> node) {
    return scene.choiceWithin(root, node);
  }

  template <class Callback>
  static std::function<void(ChoiceSelection)> choiceHandler(ManagedScene &scene, Node<Widget> node, Callback callback,
                                                            std::source_location origin) {
    return [&scene, node, lifetime = lifetime(scene),
            adapter = scene.actionAdapter<ChoiceSelection>(std::move(callback), origin),
            origin](ChoiceSelection value) mutable {
      auto result = adapter(value);
      if (!result && !lifetime.expired() && !scene.commandReported(result, lifetime)) {
        scene.remember(ManagedError{ManagedPhase::INPUT, node.id(), result.result().error(), {}, {}}, origin);
      }
    };
  }

  static void choiceModels(ManagedScene &scene, MountContext &context) {
    const auto count = scene.models_.size();
    context.rollback.push_back([&scene, count] {
      while (scene.models_.size() > count) {
        scene.models_.back()->retire();
        scene.models_.pop_back();
      }
    });
  }

  static std::expected<void, ManagedError> choiceCompanion(ManagedScene &scene, Node<Widget> owner,
                                                           Node<Widget> popup) {
    auto entry = scene.find(owner);
    if (!entry) {
      return std::unexpected(error(owner, TreeError::STALE_NODE));
    }
    entry->companion = popup;
    return {};
  }

  static Description<FindBar> findBar(std::string document, std::string query, FindBarConfig config,
                                      std::shared_ptr<portals::detail::FindModelSource> source = {},
                                      std::shared_ptr<portals::detail::FindExternalBinding> external = {});
  static std::expected<std::size_t, ManagedError> findPreparedQueries(ManagedScene &scene, Node<Widget> node);
  static std::expected<std::shared_ptr<portals::detail::FindRecord>, ManagedError> findRecord(ManagedScene &scene,
                                                                                              Node<Widget> node);
  static std::expected<FindMatch, ManagedError> findNavigate(ManagedScene &scene, Node<Widget> node,
                                                             FindDirection direction);
  static std::expected<FindMatch, ManagedError> findMatch(ManagedScene &scene, Node<Widget> node);
  static std::expected<bool, ManagedError> findCurrent(ManagedScene &scene, Node<Widget> node, const FindMatch &match);
  static std::expected<void, ManagedError> findDocument(ManagedScene &scene, Node<Widget> node,
                                                        std::string_view document);
  static std::pair<std::expected<void, TreeError>, bool> findClear(ManagedScene &scene, Node<Widget> node,
                                                                   std::source_location origin);
  static Description<FormField> formFieldComposition(
    std::string caption,
    portals::detail::MoveOnlyFunction<std::expected<Description<Widget>, ManagedError>(const ControlSettings &)> editor,
    FormFieldOptions options, std::shared_ptr<RecipeLayout> layout = {},
    std::shared_ptr<std::weak_ptr<portals::detail::FormFieldRecord>> bridge = {});
  static std::expected<void, ManagedError> connectEditValidation(ManagedScene &, Node<FormField>,
                                                                 std::shared_ptr<portals::detail::EditTextBinding>);
  static std::expected<Subscription, TreeError> observeEditNavigation(ManagedNavigation, Node<LineEdit>,
                                                                      std::function<void()>);
  static std::expected<void, ManagedError>
  bindEditText(ManagedScene &, Node<LineEdit>, std::shared_ptr<portals::detail::EditTextBinding>, LineEditConfig);
  static Description<FormField> formField(std::string caption,
                                          std::function<Description<Widget>(LineEditConfig, ControlSettings)> editor,
                                          FormFieldConfig config);
  static Description<FormField> formField(std::string caption, std::string initial, FormFieldConfig config);
  static Description<FormField> formField(std::string caption, Description<Widget> editor, FormFieldOptions options);
  static std::expected<std::shared_ptr<portals::detail::FormFieldRecord>, ManagedError> fieldRecord(ManagedScene &scene,
                                                                                                    Node<Widget> node);
  static std::expected<FieldValidation, ManagedError> fieldValidation(ManagedScene &scene, Node<Widget> node);
  static std::expected<bool, ManagedError> validateField(ManagedScene &scene, Node<Widget> node);
  static std::expected<void, ManagedError> displayField(ManagedScene &scene, Node<Widget> node, FieldValidation value);

  /// @endcond
  /** @param scene Owner of the nested mount transaction entered on this thread. */
  static void beginMount(ManagedScene &scene) noexcept {
    if (scene.owner_ == std::this_thread::get_id()) {
      ++scene.mountDepth_;
    }
  }

  /** @param scene Owner whose complete transaction, including Ref publication, has ended. */
  static void endMount(ManagedScene &scene) noexcept {
    if (scene.owner_ == std::this_thread::get_id()) {
      --scene.mountDepth_;
    }
  }

  /** @param scene Borrowed managed owner; observed only during UI-affine publication.
   * @return Weak token that expires before scene destruction releases registrations.
   */
  static std::weak_ptr<const void> lifetime(ManagedScene &scene) {
    return scene.lifetime_;
  }

  /// @cond INTERNAL
  // Keep the existing Column recipe metadata/children while using the cpp-owned dialog body adapter.
  static Description<Column> dialogContent(Description<Column> description);

  static std::expected<Node<Widget>, ManagedError> dialogPanel(ManagedScene &scene, Node<Dialog> node,
                                                               BorderOptions options) {
    return scene.makeDialogPanel(node, options);
  }

  /// @endcond

  /// @cond INTERNAL
  static std::expected<void, ManagedError> notifications(ManagedScene &scene, Node<Widget> wrapper, Node<Widget> body,
                                                         const NotificationCenter &center,
                                                         const NotificationOptions &options, MountContext &context) {
    auto reserved = NotificationAccess::reserve(center);
    if (!reserved) {
      return reserved;
    }
    try {
      context.release.emplace_back([center] {
        NotificationAccess::release(center);
      });
    } catch (...) {
      NotificationAccess::release(center);
      throw;
    }
    auto made = scene.makeNotifications(wrapper, body, options, context.nodes);
    if (!made) {
      return std::unexpected(made.error());
    }
    context.finalize.emplace_back([&scene, record = *made] {
      return scene.finalizeNotifications(record);
    });
    context.publish.emplace_back([center, record = *made] {
      NotificationAccess::publish(center, record);
    });
    return {};
  }

  /// @endcond

  /** @brief Mount a root or child inside the same transaction.
   * @tparam T Public Widget subtype constructed by the
   * recipe.
   * @param description Owning recipe consumed by this call, including on failure.
   * @param scene
   * Existing UI-affine owner of all mounted nodes.
   * @param parent Live managed parent receiving the new child.
   *
   * @param context Borrowed transaction retaining observations and deferred ref publication.
   * @return Typed
   * observing node or the unchanged creation/setup/child error.
   */
  template <class T>
  static std::expected<Node<T>, ManagedError> mount(Description<T> &&description, ManagedScene &scene,
                                                    Node<Widget> parent, MountContext &context);
  /** @tparam T Root widget type. @param description Unmounted root borrowed for bounded opt-in detection.
   * @return Whether an explicitly configured standard Grid needs the local fallback schema.
   */
  template <class T> static bool needsGridContext(const Description<T> &description);
  /// @cond INTERNAL
  template <class T> static bool needsChoiceHost(const Description<T> &description);
  template <class T> static std::expected<bool, ManagedError> prepareChoiceHost(Description<T> &, ControlContext);
  template <class T, class Probe> static Description<T> choiceProbe(Description<T>, Probe);
  template <class T> static Description<T> choiceHost(Description<T> description, bool explicitHost = false);
  /// @endcond

  /** @param description Root resource transaction validating its effective graph after local declarations.
   * @return Consuming description; native ancestor callbacks keep their existing ordering.
   */
  static Description<Widget> deferControlValidation(Description<Widget> description);
  /** @param description Root recipe borrowed before any factory effects.
   * @return Whether the resource parser owns complete effective-context validation before its effects.
   */
  template <class T> static bool defersControlValidation(const Description<T> &description);

  /** @brief Wrap a checked managed factory without exposing recipe construction publicly.
   * @tparam T Widget subtype returned by the factory.
   * @tparam Factory Owning callable returning expected<Node<T>, ManagedError> for scene and parent.
   * @param factory Capture retained until the consuming mount; construction may throw.
   * @return Single-use description with the existing transaction semantics.
   */
  template <class T, class Factory> static Description<T> describe(Factory factory);

  /** @tparam T Concrete widget created by the recipe.
   * @param description Owning recipe receiving intrinsic style names.
   * @param identity Owning natural type and required ancestors, finalized after setup modifiers.
   * @return Consuming recipe retaining the existing mount/ref transaction.
   */
  template <class T> static Description<T> intrinsicStyle(Description<T> description, StyleTarget identity);
  /** @brief Mark resource-resolved settings without budgeting the same fields twice.
   * @tparam T Concrete widget subtype retained by the recipe.
   * @param description Owning recipe updated before mounting.
   * @param alreadyMaterialized Presence mask already included in the resource mount budget.
   */
  template <class T> static void preparedControl(Description<T> &description, std::uint32_t alreadyMaterialized = 0);
  /** @brief Mark all resource-resolved fields without truncating the canonical vocabulary.
   * @tparam T Concrete widget subtype retained by the recipe.
   * @param description Owning recipe updated before mounting.
   * @param alreadyMaterialized Complete presence set already included in the resource mount budget.
   */
  template <class T>
  static void preparedControlPresence(Description<T> &description, ControlPropertyPresence alreadyMaterialized);
  /** @param description Native derived heading, whose source title was already charged.
   * @param title Owning source expression copied into presence metadata for this cold recipe.
   * Keep inherited Label defaults checked/charged while excluding the materialized caption leaf.
   */
  static void preparedDialogHeading(Description<Label> &description, const TextValue &title);
  /** @tparam T Native Label or Button. @param description Owning internal caption recipe.
   * @param caption Already materialized text expression; ordinary child defaults remain checked.
   */
  template <class T> static void preparedThemeSelectorCaption(Description<T> &description, const TextValue &caption);
  /** @param description Owning native view recipe. @return Recipe tagged with actual view capability. */
  static Description<Dialog> themeSelectorControl(Description<Dialog> description);
  /** @param description Owning native catalog recipe. @return Recipe tagged with actual catalog capability. */
  static Description<Dialog> themeCatalogControl(Description<Dialog> description);
  /** @tparam T Retained widget type. @param description Recipe inspected without mutation.
   * @return Whether its actual producer owns the catalog construction path.
   */
  template <class T> static bool themeCatalogRecipient(const Description<T> &description) noexcept;
  /** @param description Owning native message recipe. @return Recipe tagged with actual message capability. */
  static Description<Dialog> messageControl(Description<Dialog> description);
  /** @param description Owning center/body recipe. @return Recipe tagged with actual notification capability. */
  static Description<Overlay> notificationControl(Description<Overlay> description);
  /** @tparam T Retained widget type. @param description Owning native recipient recipe.
   * @param settings Effective owning resource inputs. @param materialized Already reserved leaves.
   * @param kind Required semantic capability; actual catalogs also satisfy view requirements.
   * @return Success after storing the source packet, or a typed capability mismatch.
   */
  template <class T>
  static std::expected<void, ManagedError> messageSource(Description<T> &description, const ControlSettings &settings,
                                                         ControlPropertyPresence materialized,
                                                         ControlFactoryKind kind = ControlFactoryKind::MESSAGE_BINDING);
  /** @param description Composite-owned Dialog recipe with already checked native options.
   * @param prechargedTitle Borrowed expression copied now when its leaf was already reserved.
   * @return Internal recipe retaining ordinary child defaults without inheriting Dialog defaults again.
   */
  static Description<Dialog> internalDialog(Description<Dialog> description,
                                            const TextValue *prechargedTitle = nullptr);
  /** @brief Attach a builtin configuration identity to an owning recipe.
   * @tparam T Concrete widget subtype retained by the recipe.
   * @param description Single-use recipe receiving configuration metadata.
   * @param type Canonical builtin name copied into the recipe.
   * @param inherit Whether omitted constructor properties inherit context defaults.
   * @return Recipe retaining its existing construction and cleanup behavior.
   */
  template <class T>
  static Description<T> configuredControl(Description<T> description, std::string_view type, bool inherit = true);
  /** @brief Retain explicit constructor properties above inherited defaults.
   * @tparam T Concrete widget subtype retained by the recipe.
   * @param description Single-use recipe receiving constructor metadata.
   * @param arguments Owning sparse properties supplied by the factory call.
   * @return Recipe whose later instance settings retain highest priority.
   */
  template <class T> static Description<T> controlArguments(Description<T> description, ControlSettings arguments);
  /** @tparam T Receiving widget type. @param failure Complete original prerequisite failure.
   * @return Cold failed recipe preserving the cause before any construction or binding work.
   */
  template <class T> static Description<T> failed(ManagedError failure);
  /** @tparam T Slider view type. @param description Receiving cold recipe.
   * @param patch Present resource numeric/input leaves with their original provenance.
   * Preserve factory settings for every absent resource leaf.
   */
  template <class T> static void numericSettings(Description<T> &description, ControlSettings patch);
  /** @tparam Parent Parent widget type. @tparam Child Child widget type.
   * @param parent Receiving cold recipe. @param child Consumed child recipe, inspected before transfer.
   * @param structural Retain every child relationship, including default placements.
   */
  template <class Parent, class Child>
  static void layoutChild(Description<Parent> &parent, Description<Child> &child, bool structural = false);
  /** @tparam Children Consuming description types inspected before their constructor captures are transferred.
   * @param children Ordinary child recipes, without invoking their factories.
   * @return Optional owning layout-only metadata for a composition whose children mount inside its factory.
   */
  template <class... Children> static std::shared_ptr<RecipeLayout> captureLayouts(Children &...children);
  /** @tparam T Parent widget type. @param description Cold composition retaining normal transaction behavior.
   * @param layout Previously captured owning layout metadata.
   * @return Same consuming composition with its children included in preflight.
   */
  template <class T> static Description<T> withLayout(Description<T> description, std::shared_ptr<RecipeLayout> layout);
  /** @tparam Child Consuming real child recipe. @param parent Receiving standard grid recipe.
   * @param child Single-use real child. @param region Present only for a structural section.
   */
  template <class Child>
  static void gridChild(Description<Grid> &parent, Child &&child, std::optional<GridRegion> region = {});
  /** @param parent Receiving standard grid recipe. @param section Consumed header/body group, without a Widget. */
  static void gridSection(Description<Grid> &parent, GridSection section);

  /** @param scene Live managed owner. @param node Declarative grid receiving complete identities.
   * @param options Resolved settings borrowed during checked replacement.
   * @return Existing typed configuration result, attributed to this grid.
   */
  static std::expected<void, ManagedError> gridOptions(ManagedScene &scene, Node<Grid> node,
                                                       const GridOptions &options) {
    auto result = setDeclarativeGridOptions(scene.app_.tree(), node, options);

    return result ? std::expected<void, ManagedError>{} : std::unexpected(error(node, result.error()));
  }
  /** @brief Attach binding setup after node creation within the mount transaction.
   * @tparam T Concrete widget subtype retained by the recipe.
   * @tparam Callback Synchronous checked setup callable for scene, node and settings.
   * @param description Single-use recipe retaining normal failure cleanup.
   * @param callback Owning setup callable retained until mount.
   * @return Recipe publishing references only after setup and finalization succeed.
   */
  template <class T, class Callback> static Description<T> controlSetup(Description<T> description, Callback callback);
  /** @param description Typed factory recipe. @param patch Present Switch resource leaves and their origins.
   * Preserve all other factory settings; explicit resource false values are overrides.
   */
  static void switchSettings(Description<Switch> &description, ControlSettings patch);
  /** @param options Existing owning native frame options.
   * @param style Existing owning manual appearances.
   * @return Empty native frame recipe; callers attach exactly one real content edge without a wrapper.
   */
  static Description<Border> borderControl(BorderOptions options, ControlStyle style);
  /** @param options Owning advanced appearance, validated before constructing descendants.
   * @param style Manual owning appearances beneath the existing stylesheet resolver.
   * @return Border recipe participating in ordinary mount tracking, rollback and reference publication.
   */
  static Description<Border> framedControl(FrameOptions options, ControlStyle style);
  /** @brief Stage precharged frame settings on the exact typed source recipe.
   * @param description Recipe retaining its ordinary source identity and callbacks on T.
   * @param settings Owning sparse frame patch and diagnostic provenance copied before erasure.
   */
  template <class T> static void frameSettings(Description<T> &description, const ControlSettings &settings);
  /** @brief Attach an owning custom body to a resultful dialog recipe.
   * @param description Message-bound recipe retaining its source identity.
   * @param body Cold content mounted once under the managed message owner.
   * @return Success, or a typed error when the recipe cannot accept the body.
   */
  template <class T>
  static std::expected<void, ManagedError> messageBody(Description<T> &description, Description<Widget> body);
  /** @param options Actual ordinary Overlay alignment/insets.
   * @param policy Actual parent size policy, preserved when sparse settings are absent.
   * @return Empty native Overlay recipe; children attach as real edges in source order.
   */
  static Description<Overlay> overlayControl(OverlayOptions options, SizePolicy policy);
  /** @param scene Owning UI scene. @param node Newly constructed scroll control.
   * @param settings Effective sparse owning configuration. @return Prepared skin or existing typed error.
   */
  static std::expected<void, ManagedError> applyScrollbarSkin(ManagedScene &scene, Node<Widget> node,
                                                              const ControlSettings &settings);

  /** @param options Actual owning placement/input/frame options.
   * @param controller Optional owning shared values; an empty area controller keeps native default creation.
   * @param style Owning manual appearances beneath the existing theme.
   * @return Native area recipe without a synthetic child; caller attaches one real content edge.
   */
  static Description<ScrollArea> scrollAreaControl(ScrollAreaOptions options,
                                                   std::shared_ptr<ScrollController> controller, ControlStyle style);
  /** @param controller Optional only for a standalone writer; a projection requires an owning shared controller.
   * @param axis Actual receiving projection axis.
   * @param options Actual receiving input/paint options.
   * @param style Owning manual appearances.
   * @param writer Existing native standalone geometry-writer role; false preserves the shared projection role.
   * @return Native bar recipe with checked sparse settings and unchanged controller ownership.
   */
  static Description<ScrollBar> scrollBarControl(std::shared_ptr<ScrollController> controller, ScrollAxis axis,
                                                 ScrollBarOptions options, ControlStyle style, bool writer);

  /** @brief Identify builtin recipe types without allocating runtime metadata.
   * @tparam T Concrete widget subtype to classify.
   * @return Canonical builtin configuration name, or an empty view for custom types.
   */
  template <class T> static constexpr std::string_view controlType() {
    if constexpr (std::same_as<T, Label>) {
      return "Label";
    } else if constexpr (std::same_as<T, Button>) {
      return "Button";
    } else if constexpr (std::same_as<T, CheckBox>) {
      return "CheckBox";
    } else if constexpr (std::same_as<T, Switch>) {
      return "Switch";
    } else if constexpr (std::same_as<T, ThreeState>) {
      return "ThreeState";
    } else if constexpr (std::same_as<T, FormField>) {
      return "FormField";
    } else if constexpr (std::same_as<T, FindBar>) {
      return "FindBar";
    } else if constexpr (std::same_as<T, LineEdit>) {
      return "LineEdit";
    } else if constexpr (std::same_as<T, Password>) {
      return "Password";
    } else if constexpr (std::same_as<T, Row>) {
      return "HBox";
    } else if constexpr (std::same_as<T, RadioGroup>) {
      return "RadioGroup";
    } else if constexpr (std::same_as<T, StatusBar>) {
      return "StatusBar";
    } else if constexpr (std::same_as<T, Column>) {
      return "VBox";
    } else if constexpr (std::same_as<T, Border>) {
      return "Border";
    } else if constexpr (std::same_as<T, Gauge>) {
      return "Gauge";
    } else if constexpr (std::same_as<T, SpinBox>) {
      return "SpinBox";
    } else if constexpr (std::same_as<T, Slider>) {
      return "Slider";
    } else if constexpr (std::same_as<T, ScrollArea>) {
      return "ScrollArea";
    } else if constexpr (std::same_as<T, DocumentView>) {
      return "DocumentView";
    } else if constexpr (std::same_as<T, LogView>) {
      return "LogView";
    } else if constexpr (std::same_as<T, ScrollBar>) {
      return "ScrollBar";
    } else if constexpr (std::same_as<T, HelpView>) {
      return "HelpView";
    } else if constexpr (std::same_as<T, KeyHints>) {
      return "KeyHints";
    } else if constexpr (IsVirtualList<T>::value) {
      return "VirtualList";
    } else if constexpr (IsVirtualTable<T>::value) {
      return "VirtualTable";
    } else if constexpr (IsChart<T>::value) {
      return "Chart";
    } else if constexpr (std::same_as<T, DataStateView>) {
      return "DataStateView";
    } else if constexpr (std::same_as<T, OverlayHost>) {
      return "OverlayHost";
    } else if constexpr (std::same_as<T, Dialog>) {
      return "Dialog";
    } else if constexpr (std::same_as<T, Overlay>) {
      return "Overlay";
    } else
      return {};
  }

  /** @param base Owning lower-priority constructor arguments.
   * @param patch Final explicit sparse instance properties.
   * @return Owning structural merge, retaining explicit false/zero and winning property provenance.
   */
  static ControlSettings mergeControlSettings(ControlSettings base, const ControlSettings &patch) {
    mergeRecipeControlSettings(base, patch);
    return base;
  }

  /** @tparam T Exact native Chart specialization. @param description Factory result before erasure.
   * @param patch Chart-only scalar patch; old text metadata and services are excluded by the caller.
   * Stages the same sparse merge and local provenance as native modifiers.
   */
  template <class T>
    requires IsChart<T>::value
  static void chartSettings(Description<T> &description, ControlSettings patch) {
    (void)std::move(description).scalarSettings(std::move(patch));
  }

  /** @param scene Existing UI owner; checked before reading mutable metadata.
   * @param node Live managed node whose latest target is retained during preparation.
   * @param identity Owned natural type/ancestors retained by the mounting recipe.
   * @return Checked style admission; metadata copies and new ancestors are prepared before publication.
   */
  static std::expected<void, ManagedError> finalizeStyleIdentity(ManagedScene &scene, Node<Widget> node,
                                                                 const StyleTarget &identity) {
    auto allowed = scene.boundary(ManagedPhase::STYLE, node);

    if (!allowed) {
      return allowed;
    }

    auto entry = scene.find(node);

    if (!entry) {
      return std::unexpected(ManagedError{ManagedPhase::STYLE, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }

    try {
      // Retain the exact metadata owner across allocation and possible nested mutation.
      const auto current = entry->target;

      if (current && current->baseTypes.size() > MAX_STYLE_BASE_TYPES) {
        return std::unexpected(
          ManagedError{ManagedPhase::STYLE, node.id(), validateStyleTarget(*current).error(), {}, {}});
      }

      if (current && !current->type.empty() && (!identity.control || current->control)) {
        const auto containsName = [&](const std::string &name) {
          return name == current->type || std::ranges::find(current->baseTypes, name) != current->baseTypes.end();
        };

        if (containsName(identity.type) && std::ranges::all_of(identity.baseTypes, containsName)) {
          auto valid = validateStyleTarget(*current);

          if (!valid) {
            return std::unexpected(ManagedError{ManagedPhase::STYLE, node.id(), valid.error(), {}, {}});
          }

          return {};
        }
      }

      auto target = current ? *current : StyleTarget{};

      if (target.type.empty()) {
        target.type = identity.type;
      }

      const auto retainName = [&](const std::string &name) {
        if (name != target.type && std::ranges::find(target.baseTypes, name) == target.baseTypes.end()) {
          target.baseTypes.push_back(name);
        }
      };

      retainName(identity.type);

      for (const auto &name : identity.baseTypes) {
        retainName(name);
      }

      target.control = target.control || identity.control;

      auto valid = validateStyleTarget(target);

      if (!valid) {
        return std::unexpected(ManagedError{ManagedPhase::STYLE, node.id(), valid.error(), {}, {}});
      }

      return scene.style(node, std::move(target));
    } catch (const std::bad_alloc &) {
      return std::unexpected(ManagedError{ManagedPhase::STYLE, node.id(), TreeError::NO_MEMORY, {}, {}});
    }
  }

  /// @cond INTERNAL
  template <class T, class Factory> static Description<T> cold(Factory factory);

  static void beginCold(ManagedScene &scene) noexcept {
    ++scene.coldMountPending_;
  }

  struct ColdGraph {
    ManagedScene *scene{};
    std::shared_ptr<ManagedScene::LiveTextRecord> live;
    std::shared_ptr<void> output;
    std::shared_ptr<ManagedScene::ObserverRecord> observer;
    std::shared_ptr<portals::detail::ManagedModelSource> model;
    bool pending{};

    void retire() noexcept {
      // The complete logical mark precedes disconnect/destructor reentry.
      if (pending) {
        --scene->coldMountPending_;
        pending = false;
      }
      if (observer) {
        observer->retired = true;
      }
      if (observer) {
        observer->subscription = {};
      }
      if (model) {
        std::erase(scene->models_, model);
      }
      if (observer) {
        std::erase(scene->observers_, observer);
      }
      if (output) {
        std::erase(scene->properties_, output);
        std::erase_if(scene->originProperties_, [&](const auto &record) {
          return record.property.lock() == output;
        });
      }
      if (live) {
        live->origin->retired = true;
        scene->pruneLiveText();
      }
    }
  };

  static std::shared_ptr<RefTarget> captionIdentity(const TextSource &source) {
    return source.target_;
  }

  static std::shared_ptr<RefTarget> captionTarget(std::shared_ptr<RefTarget> target, MountContext &context) {
    for (const auto &pending : context.pending) {
      if (pending.identity == target.get()) {
        return std::make_shared<RefTarget>(pending.target);
      }
    }
    return target;
  }

  static std::expected<void, ManagedError> finishCaptions(MountContext &context) {
    if (std::ranges::all_of(context.captions, &MountContext::CaptionFinalizer::completed)) {
      return {};
    }
    // Resolve dependencies once; iterative completion never consumes one stack frame per control.
    std::vector<std::optional<std::size_t>> dependencies(context.captions.size());
    for (std::size_t index = 0; index < context.captions.size(); ++index) {
      auto target = captionTarget(context.captions[index].source, context);
      if (!target) {
        return std::unexpected(error(context.captions[index].receiver, TreeError::CLOSED));
      }
      if (target->owner != std::this_thread::get_id()) {
        return std::unexpected(error(context.captions[index].receiver, TreeError::WRONG_THREAD));
      }
      for (std::size_t source = 0; source < context.captions.size(); ++source) {
        if (context.captions[source].receiver == target->node) {
          dependencies[index] = source;
          break;
        }
      }
    }
    auto remaining = std::ranges::count(context.captions, false, &MountContext::CaptionFinalizer::completed);
    while (remaining) {
      bool progressed{};
      for (std::size_t index = 0; index < context.captions.size(); ++index) {
        auto &caption = context.captions[index];
        if (caption.completed || (dependencies[index] && !context.captions[*dependencies[index]].completed)) {
          continue;
        }
        auto completed = caption.complete();
        if (!completed) {
          return completed;
        }
        caption.completed = true;
        --remaining;
        progressed = true;
      }
      if (!progressed) {
        const auto cycle = std::ranges::find(context.captions, false, &MountContext::CaptionFinalizer::completed);
        return std::unexpected(error(cycle->receiver, TreeError::INVALID_ARGUMENT));
      }
    }
    return {};
  }

  static std::expected<void, ManagedError> completeCaption(ManagedScene &scene, Node<Control> node,
                                                           const TextSource &source, MountContext &context,
                                                           const std::shared_ptr<ColdGraph> &graph) {
    auto target = captionTarget(source.target_, context);
    if (!target) {
      return std::unexpected(error(node, TreeError::CLOSED));
    }
    if (target->owner != std::this_thread::get_id()) {
      return std::unexpected(error(node, TreeError::WRONG_THREAD));
    }
    auto owner = eventScene(target);
    if (!owner) {
      return std::unexpected(owner.error());
    }
    if (*owner != &scene) {
      return std::unexpected(error(node, TreeError::INVALID_ARGUMENT));
    }
    const bool existed = std::ranges::any_of(scene.liveText_, [&](const auto &record) {
      return record->origin->node == target->node && record->kind == static_cast<int>(source.kind_);
    });
    auto live =
      scene.attachLiveText(TextSource(target, source.kind_), PropertyOptions{}, std::source_location::current(), false);
    if (!live) {
      return std::unexpected(live.error());
    }
    if (!existed) {
      const auto created = std::ranges::find_if(scene.liveText_, [&](const auto &record) {
        return record->origin == live->state_.origin_;
      });
      if (created != scene.liveText_.end()) {
        graph->live = *created;
      }
    }
    // A shared capture can precede this mount; read the original identity for a fresh initial caption.
    auto initial = TextSource(target, source.kind_).snapshot();
    if (!initial) {
      return std::unexpected(initial.error());
    }
    auto applied = setControlText(scene.app_.tree(), node, std::move(*initial));
    if (!applied) {
      return std::unexpected(error(node, applied.error()));
    }
    auto observed = scene.observe(node, *live, [&scene, node](const GraphemeText &text) {
      return setControlText(scene.app_.tree(), node, text);
    });
    if (!observed) {
      return observed;
    }
    graph->observer = scene.observers_.back();
    graph->observer->operation = "text.caption";
    if (graph->pending) {
      --scene.coldMountPending_;
      graph->pending = false;
    }
    return {};
  }

  static std::expected<void, ManagedError> completeTextList(ManagedScene &scene, Node<VirtualList<TextSegment>> node,
                                                            const TextSource &source, bool splitWords,
                                                            TextSplitOptions options, MountContext &context,
                                                            const std::shared_ptr<ColdGraph> &graph,
                                                            ManagedViewHandler<TextSegment> prepared) {
    auto target = source.target_;
    for (const auto &pending : context.pending) {
      if (pending.identity == target.get()) {
        target = std::make_shared<RefTarget>(pending.target);
        break;
      }
    }
    if (target && target->owner != std::this_thread::get_id()) {
      return std::unexpected(error({}, TreeError::WRONG_THREAD));
    }
    if (!target) {
      return std::unexpected(error({}, TreeError::CLOSED));
    }
    const bool existed = std::ranges::any_of(scene.liveText_, [&](const auto &record) {
      return record->origin->node == target->node && record->kind == static_cast<int>(source.kind_);
    });
    auto resolved = TextSource(target, source.kind_);
    auto live = scene.attachLiveText(resolved, PropertyOptions{}, std::source_location::current(), false);
    if (!live) {
      return std::unexpected(live.error());
    }
    const auto existing = std::ranges::find_if(scene.liveText_, [&](const auto &record) {
      return record->origin == live->state_.origin_;
    });
    // Ownership is assigned by the finalizer that created the bridge, not by every shared consumer.
    if (!existed && existing != scene.liveText_.end()) {
      graph->live = *existing;
    }
    auto initialText = live->snapshot();
    if (!initialText) {
      return std::unexpected(error(target->node, initialText.error()));
    }
    if ((*initialText)->value.policy() != scene.widthPolicy()) {
      return std::unexpected(error(target->node, TreeError::INVALID_ARGUMENT));
    }
    auto admitted = scene.observerAdmission(node);
    if (!admitted) {
      return std::unexpected(admitted.error());
    }
    auto textFailure = std::make_shared<std::optional<TextError>>();
    auto derived = scene.derive(*live, [splitWords, options, textFailure](const GraphemeText &text) {
      *textFailure = portals::detail::textSplitLimit(text, options.limits);
      return splitWords ? portals::words(text, options) : portals::graphemes(text, options);
    });
    if (!derived) {
      auto failure = derived.error();
      failure.node = target->node.id();
      failure.text = *textFailure;
      return std::unexpected(failure);
    }
    graph->output = *derived->state_.lock();
    graph->observer = scene.observers_.back();
    graph->observer->textFailure = std::move(textFailure);
    graph->observer->operation = splitWords ? "text.words" : "text.graphemes";
    graph->observer->node = node;
    scene.originProperties_.push_back({live->state_.origin_, graph->output, node});
    // Private cold edges share origin retirement; ordinary public derived outputs remain independent.
    derived->state_.origin_ = live->state_.origin_;
    auto model =
      scene.collectionModel<TextSegment>(derived->state_, CollectionModelOptions{}, portals::detail::segmentRows);
    if (!model) {
      return std::unexpected(model.error());
    }
    auto owned = model->lock();
    if (!owned) {
      return std::unexpected(error(node, owned.error()));
    }
    graph->model = *owned;
    graph->model->receiver = node;
    auto range = (*owned)->model->openRange();
    if (!range) {
      return std::unexpected(error(node, range.error()));
    }
    auto installed = scene.app_.tree().edit(node, [&](VirtualList<TextSegment> &view) {
      view.consumer_ = std::move(*range);
    });
    if (!installed) {
      return std::unexpected(error(node, installed.error()));
    }
    auto attached = scene.attachView(node, *owned, std::move(prepared));
    if (attached && graph->pending) {
      --scene.coldMountPending_;
      graph->pending = false;
    }
    return attached ? std::expected<void, ManagedError>{} : std::unexpected(attached.error());
  }

  /// @endcond

  /** @brief Build the shared checked data-presentation recipe for public overloads.
   * @tparam Content Retained body widget type.
   * @tparam Initial Owning presentation value or same-scene managed source.
   * @param initial Initial value or source retained by the consuming recipe.
   * @param content Single-use body recipe mounted in the same transaction.
   * @param config Owning construction-only labels, styles and retry action.
   * @param automatic Read the virtual body's prepared status when true.
   * @return Single-use composition preserving mount rollback and deferred reference publication.
   */
  template <class Content, class Initial>
  static Description<DataStateView> dataState(Initial initial, Description<Content> content, DataStateConfig config,
                                              bool automatic);

  /** @tparam Factory Owning checked callable accepting scene, parent and the shared mount context.
   * @param factory Internal adapter performing a complete existing recipe transaction.
   * @return Single-use erased recipe; no extra widget or lifecycle owner is created.
   */
  template <class Factory> static Description<Widget> transaction(Factory factory);

  /** @brief Erase only the observer type while preserving one mount transaction.
   * @tparam T Concrete public widget type produced by the existing recipe.
   * @param description Owning typed recipe consumed once.
   * @return Single-use recipe sharing rollback and deferred reference publication.
   */
  template <class T> static Description<Widget> erase(Description<T> description);
  /** @param description Consuming composite recipe whose factory forwards an inner mount's layout root.
   * @return Same source recipe opting into synchronous outer-root propagation; no retained wrapper is added.
   */
  static Description<Widget> forwardOuter(Description<Widget> description);

  /** @param scene Existing managed owner receiving a checked policy update.
   * @param node Live sized target created by this mount transaction.
   * @param change Owning operation; other axes and unrelated bounds are preserved.
   * @return Original managed boundary/tree/validation error, or success.
   */
  static std::expected<void, ManagedError> resize(ManagedScene &scene, Node<SizedWidget> node, SizeChange change) {
    const auto allowed = scene.boundary(ManagedPhase::MOUNT, node);

    if (!allowed) {
      return allowed;
    }

    SizePolicy policy;
    auto &tree = scene.app_.tree();
    const auto read = tree.read(node, [&](const SizedWidget &widget) {
      policy = widget.sizePolicy();
    });

    if (!read) {
      return std::unexpected(error(node, read.error()));
    }

    if (change.dimension == SizeDimension::WIDTH) {
      policy.width = {change.extent, change.extent, change.extent, SizeMode::FIXED, 1};
    } else if (change.dimension == SizeDimension::HEIGHT) {
      policy.height = {change.extent, change.extent, change.extent, SizeMode::FIXED, 1};
    } else {
      if (change.dimension != SizeDimension::FLEX_Y) {
        policy.width.mode = SizeMode::FLEX;
        policy.width.preferred = 0;
        policy.width.weight = change.weight;
      }

      if (change.dimension != SizeDimension::FLEX_X) {
        policy.height.mode = SizeMode::FLEX;
        policy.height.preferred = 0;
        policy.height.weight = change.weight;
      }
    }

    const auto changed = setSizePolicy(tree, node, policy);

    return changed ? std::expected<void, ManagedError>{} : std::unexpected(error(node, changed.error()));
  }

  /** @brief Stage sizing. @param description Cold recipe. @param sizing Copied owning overlay. */
  template <class T> static void sizingSettings(Description<T> &description, const RecipeSizing &sizing);

  /** @brief Commit final cold bounds and transparent frame preferences in one checked update.
   * @param scene UI-affine mount owner. @param source Concrete sized content.
   * @param target Outer frame when present, otherwise source itself.
   * @param sizing Optional final sparse instance geometry; borrowed only for this call.
   * @return Unchanged typed read, lifetime, policy or invalidation failure.
   */
  static std::expected<void, ManagedError> finishSizing(ManagedScene &scene, Node<SizedWidget> source,
                                                        Node<SizedWidget> target, const RecipeSizing *sizing) {
    SizePolicy policy;
    auto &tree = scene.app_.tree();
    const auto read = tree.read(target, [&](const SizedWidget &widget) {
      policy = widget.sizePolicy();
    });

    if (!read) {
      return std::unexpected(error(target, read.error()));
    }

    if (source != target) {
      SizePolicy content;
      const auto inspected = tree.read(source, [&](const SizedWidget &widget) {
        content = widget.sizePolicy();
      });

      if (!inspected) {
        return std::unexpected(error(source, inspected.error()));
      }

      const auto inherit = [](AxisSize &outer, AxisSize inner) {
        if (outer == AxisSize{} && (inner.mode == SizeMode::CONTENT || inner.mode == SizeMode::FLEX)) {
          // Numeric content bounds describe another box; only its layout preference crosses the frame.
          outer.mode = inner.mode;
          outer.preferred = inner.preferred;
          outer.weight = inner.weight;
        }
      };
      inherit(policy.width, content.width);
      inherit(policy.height, content.height);
    }

    if (sizing) {
      policy = sizing->apply(policy);
    }

    const auto changed = setSizePolicy(tree, target, policy);
    return changed ? std::expected<void, ManagedError>{} : std::unexpected(error(target, changed.error()));
  }

  /** @brief Apply final local instance flags through the existing managed mount boundary.
   * @param scene Borrowed managed owner of the current transaction.
   * @param node Created target; ancestor flags retain their existing tree semantics.
   * @param visible Optional final local visibility.
   * @param enabled Optional final local enabled state.
   * @return Original boundary or tree failure attributed to this node in MOUNT.
   */
  static std::expected<void, ManagedError> instanceState(ManagedScene &scene, Node<Widget> node,
                                                         std::optional<bool> visible, std::optional<bool> enabled) {
    const auto allowed = scene.boundary(ManagedPhase::MOUNT, node);

    if (!allowed) {
      return allowed;
    }

    auto &tree = scene.app_.tree();

    if (visible) {
      const auto changed = tree.setVisible(node, *visible);

      if (!changed) {
        return std::unexpected(error(node, changed.error()));
      }
    }

    if (enabled) {
      const auto changed = tree.setEnabled(node, *enabled);

      if (!changed) {
        return std::unexpected(error(node, changed.error()));
      }
    }

    return {};
  }

  /** @brief Verify a created node remains accessible before publishing references.
   * @param scene Existing managed
   * owner, borrowed for this check.
   * @param node Observing identity whose tree scope is validated.
   * @return
   * Success or the original managed boundary/liveness error.
   */
  static std::expected<void, ManagedError> check(ManagedScene &scene, Node<Widget> node) {
    return scene.boundary(ManagedPhase::MOUNT, node);
  }

  /** @param scene Existing owner with a completely mounted explicit or automatic host.
   * @param root Exact recipe root which must be that owner's registered host.
   * @return The existing typed host or INVALID_ARGUMENT; never creates a second host.
   */
  static std::expected<Node<OverlayHost>, ManagedError> layerHost(ManagedScene &scene, Node<Widget> root);

  /** @param scene Existing owner of the selected host and unpublished layer.
   * @param node Mounted recipe root; erased recipes must produce an exact bound Dialog.
   * @return Success for an existing registered Dialog or typed INVALID_ARGUMENT.
   */
  static std::expected<void, ManagedError> checkLayer(ManagedScene &scene, Node<Widget> node);
};
} // namespace detail

} // namespace portals::ui
