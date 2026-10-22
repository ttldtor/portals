// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui.hpp>

#include <filesystem>

namespace portals::ui {
struct SceneResourceOptions;

/** @brief Explicit grammar selection; registered node names never select a format implicitly. */
enum class SceneResourceFormat {
  LEGACY,       ///< Existing single-root resource document.
  CONFIGURATION ///< Explicit definitions/defaults/scene blocks and nested typed settings.
};

/** @brief Finite parser budgets, checked before calling any registered factory. */
struct SceneResourceLimits {
  std::size_t maxSourceBytes{1024 * 1024}; ///< Positive maximum UTF-8 source bytes.
  std::size_t maxNodes{4096};              ///< Positive maximum node declarations.
  std::size_t maxDepth{64};                ///< Positive maximum node depth, at most 64; root depth is one.
  std::size_t maxStringBytes{64 * 1024};   ///< Positive maximum decoded bytes in one string or resource key.
  std::size_t maxProperties{16};           ///< Positive maximum named/positional properties per node.
};

/** @brief Owning primitive inputs to a registered typed recipe factory; no evaluated expressions. */
struct SceneResourceArguments {
  /** @brief Keep transfers fallible: checked MSVC string moves may allocate a debug proxy. */
  ~SceneResourceArguments() = default;

  std::string text = std::string(0, '\0'); ///< Resolved owning UTF-8 text; empty when omitted.
  std::string id = std::string(0, '\0');   ///< Optional owning case-sensitive node id.
  std::optional<std::int32_t> width;       ///< Nonnegative fixed cell width, applied after the factory recipe.
  std::optional<std::int32_t> height;      ///< Nonnegative fixed cell height, applied after the factory recipe.
  std::optional<std::uint32_t> flex;       ///< Positive existing flex weight, applied after width/height.
  std::int32_t gap{};                      ///< Nonnegative layout gap; resource syntax admits this only on VBox/HBox.
  bool checked{};                          ///< Initial checkbox state; resource syntax admits this only on CheckBox.
  ControlSettings settings{};              ///< Validated effective sparse settings; no IDs, models or handlers.
  std::optional<bool> visible;             ///< Local visibility applied after the factory's recipe modifiers.
  std::optional<bool> enabled;             ///< Local enabled state applied after the factory's recipe modifiers.
  std::optional<std::string> classes; ///< Owning literal class list; absent preserves factory classes, empty clears.
};

namespace detail {
struct ResourceAccess;

/** @brief Allocation-free sticky fluent-registration outcome; not user-configurable. */
class ResourceRegistrationState {
  friend struct portals::ui::SceneResourceOptions;
  friend struct ResourceAccess;

  std::optional<std::pair<SceneResourceErrorCode, TreeError>> failure_;
};

/** @brief Exact typed recipe factory result accepted by a resource registry.
 * @tparam T Concrete public Widget subtype.
 * @tparam Factory Copyable owning callable accepting SceneResourceArguments by value.
 */
template <class T, class Factory>
concept ResourceFactory =
  std::derived_from<T, Widget> && std::copy_constructible<Factory> &&
  requires(Factory &factory, SceneResourceArguments arguments) {
    requires std::same_as<std::invoke_result_t<Factory &, SceneResourceArguments>, Description<T>> ||
               std::same_as<std::invoke_result_t<Factory &, SceneResourceArguments>,
                            std::expected<Description<T>, ManagedError>>;
    std::invoke(factory, std::move(arguments));
  };
} // namespace detail

/** @brief Explicit owning allowlist; builtins are VBox/HBox/Label/LineEdit/Button/CheckBox/ThreeState.
 * Construction and copying may allocate. Captured services remain explicit borrows and must
 * outlive prepare; registry callbacks are released with the consumed scene recipe.
 */
class SceneResourceRegistry {
public:
  /** @brief Register the seven safe builtin recipe adapters without mounting anything. */
  SceneResourceRegistry();

  /** @brief Release owning adapters; copying remains fallible instead of a noexcept debug-vector move. */
  ~SceneResourceRegistry() = default;

  /** @brief Register an explicitly typed custom recipe; existing names cannot be replaced.
   * @tparam T Public Widget subtype returned by the factory, including final types.
   * @tparam Factory Copyable owning callable accepting owning SceneResourceArguments, returning
   * Description<T> or exactly expected<Description<T>, ManagedError>.
   * @param name ASCII identifier copied now; case-sensitive and unique, including builtin names.
   * @param factory Invoked once per node at prepare, after complete syntax/schema/id validation.
   * External side effects are not rolled back; exceptions use the existing checked mount boundary.
   * In CONFIGURATION, exact VirtualList/VirtualTable/Chart registrations accept their applicable scalar settings;
   * these are staged after the factory, like common size properties. Models and formatters remain code captures.
   * @param children Whether this custom type accepts child resource nodes; defaults to a leaf.
   * @return Success, UNKNOWN_TYPE for an invalid name, or DUPLICATE_TYPE for a registered name.
   * Allocation/capture construction before and during registration may throw.
   */
  template <class T, class Factory>
    requires detail::ResourceFactory<T, Factory>
  std::expected<void, SceneResourceErrorCode> add(std::string_view name, Factory factory, bool children = false) {
    return insert(makeEntry<T>(name, std::move(factory), children));
  }

  /** @brief Explicitly bind the existing native MessageBox/Notifications/ThemeSelector/ThemeSelectorCatalog semantic
   * schema to a real code-owned factory.
   * @tparam T Public Widget result type. @tparam Factory Existing owning resource callable.
   * @tparam Schema Exactly the metadata span returned by controlPropertySchema; deduction preserves old bool calls.
   * @param name Unique owning registration name. @param factory Actual code-owned native composite recipe producer.
   * @param schema Complete canonical MessageBox, Notifications, ThemeSelector or ThemeSelectorCatalog descriptors, read
   * only during this call and never retained.
   * @return Checked registration, or INVALID_VALUE for empty/foreign/modified metadata.
   * A returned ordinary/wrong native recipe is rejected during prepare; semantic resource nodes are leaves.
   */
  template <class T, class Factory, class Schema>
    requires detail::ResourceFactory<T, Factory> &&
             std::same_as<std::remove_cvref_t<Schema>, std::span<const ControlPropertyDescriptor>>
  std::expected<void, SceneResourceErrorCode> add(std::string_view name, Factory factory, Schema &&schema) {
    const auto semantic = semanticSchema(schema);

    if (!semantic) {
      return std::unexpected(SceneResourceErrorCode::INVALID_VALUE);
    }

    auto entry = makeEntry<T>(name, std::move(factory), false, *semantic);

    entry.text = false;
    entry.sized = *semantic == detail::ControlFactoryKind::NOTIFICATION_BINDING;
    entry.factoryKind = *semantic;

    return insert(std::move(entry));
  }

private:
  friend struct detail::ResourceAccess;

  static std::optional<detail::ControlFactoryKind>
  semanticSchema(std::span<const ControlPropertyDescriptor> schema) noexcept;

  struct Entry {
    ~Entry() = default;

    std::string name = std::string(0, '\0');
    bool children{};
    bool sized{};
    bool text{true};
    bool gap{};
    bool checked{};
    bool declaredHelp{};
    bool unknownHelp{};
    bool declaredDialog{};
    bool declaredHost{};
    bool unknownDialog{};
    std::string_view virtualView{};
    detail::ControlFactoryKind factoryKind{detail::ControlFactoryKind::USER};
    std::function<std::expected<Description<Widget>, ManagedError>(SceneResourceArguments)> create;
  };

  // Shared cold adapter construction; public add<T> retains its exact factory and common-property semantics.
  template <class T, class Factory>
    requires detail::ResourceFactory<T, std::remove_cvref_t<Factory>>
  static Entry makeEntry(std::string_view name, Factory &&factory, bool children = false,
                         detail::ControlFactoryKind semantic = detail::ControlFactoryKind::USER) {
    Entry entry;

    entry.name = name;
    entry.children = children;
    entry.sized = std::derived_from<T, SizedWidget>;
    entry.declaredHelp = std::derived_from<T, HelpView>;
    entry.unknownHelp = std::same_as<T, Widget>;
    entry.declaredDialog = std::derived_from<T, Dialog>;
    entry.declaredHost = std::derived_from<T, OverlayHost>;
    entry.unknownDialog = std::same_as<T, Widget>;

    if constexpr (detail::IsVirtualList<T>::value || detail::IsVirtualTable<T>::value || detail::IsChart<T>::value) {
      entry.virtualView = detail::Access::controlType<T>();
    }

    entry.create = [factory = std::forward<Factory>(factory), semantic](
                     SceneResourceArguments arguments) mutable -> std::expected<Description<Widget>, ManagedError> {
      // Common properties remain existing typed recipe operations before erasure.
      const auto width = arguments.width;
      const auto height = arguments.height;
      const auto flex = arguments.flex;
      const auto visible = arguments.visible;
      const auto enabled = arguments.enabled;
      const auto classes = arguments.classes;
      const auto materialized = detail::controlSettingsPresenceSet(arguments.settings);
      [[maybe_unused]] const auto rowHeight = arguments.settings.rowHeight;
      [[maybe_unused]] const auto overscanRows = arguments.settings.overscanRows;
      [[maybe_unused]] const auto maxColumns = arguments.settings.maxColumns;
      [[maybe_unused]] const auto maxPreparedCells = arguments.settings.maxPreparedCells;
      [[maybe_unused]] const auto maxPreparedTextBytes = arguments.settings.maxPreparedTextBytes;
      [[maybe_unused]] const auto mode = arguments.settings.mode;
      [[maybe_unused]] const auto chartGlyphSet = arguments.settings.chartGlyphSet;
      [[maybe_unused]] const auto candleGap = arguments.settings.candleGap;
      [[maybe_unused]] const auto yRange = arguments.settings.yRange;
      std::optional<ControlSettings> semanticSettings;

      if (semantic != detail::ControlFactoryKind::USER) {
        semanticSettings.emplace(arguments.settings);
      }

      auto id = arguments.id;
      auto recipe = std::expected<Description<T>, ManagedError>(std::invoke(factory, std::move(arguments)));

      if (!recipe) {
        return std::unexpected(recipe.error());
      }

      if (semanticSettings) {
        auto staged = detail::Access::messageSource(*recipe, *semanticSettings, materialized, semantic);

        if (!staged) {
          return std::unexpected(staged.error());
        }
      }

      detail::Access::preparedControlPresence(*recipe, materialized);

      // Only exact data families consume these literals; old text metadata remains factory-owned.
      if constexpr (detail::IsVirtualList<T>::value || detail::IsVirtualTable<T>::value) {
        if (rowHeight) {
          (void)std::move(*recipe).rowHeight(*rowHeight);
        }

        if (overscanRows) {
          (void)std::move(*recipe).overscanRows(*overscanRows);
        }
      }

      if constexpr (detail::IsVirtualTable<T>::value) {
        if (maxColumns) {
          (void)std::move(*recipe).maxColumns(*maxColumns);
        }

        if (maxPreparedCells) {
          (void)std::move(*recipe).maxPreparedCells(*maxPreparedCells);
        }

        if (maxPreparedTextBytes) {
          (void)std::move(*recipe).maxPreparedTextBytes(*maxPreparedTextBytes);
        }
      }

      if constexpr (detail::IsChart<T>::value) {
        detail::Access::chartSettings(
          *recipe, {.mode = mode, .chartGlyphSet = chartGlyphSet, .candleGap = candleGap, .yRange = yRange});
      }

      if (visible) {
        (void)std::move(*recipe).visible(*visible);
      }

      if (enabled) {
        (void)std::move(*recipe).enabled(*enabled);
      }

      if (classes) {
        (void)std::move(*recipe).classes(*classes);
      }

      if (!id.empty()) {
        (void)std::move(*recipe).id(id);
      }

      if constexpr (std::derived_from<T, SizedWidget>) {
        // Native semantic Notifications applies these precharged leaves through its actual resolved Overlay once.
        if (semantic != detail::ControlFactoryKind::NOTIFICATION_BINDING) {
          if (width) {
            (void)std::move(*recipe).width(*width);
          }

          if (height) {
            (void)std::move(*recipe).height(*height);
          }

          if (flex) {
            (void)std::move(*recipe).flex(*flex);
          }
        }
      }

      return detail::Access::erase(std::move(*recipe));
    };

    return entry;
  }

  std::expected<void, SceneResourceErrorCode> insert(Entry entry);
  std::vector<Entry> entries_ = std::vector<Entry>(0);
};

/** @brief Explicit resource-loader configuration; no process-global registry or locale.
 * Configure without concurrent access, then transfer/copy into a scene on its UI thread.
 * Factories own their captures; referenced services must outlive synchronous prepare.
 * Copies retain the first fluent registration failure; checked registry.add failures are independent.
 */
struct SceneResourceOptions {
  SceneResourceRegistry registry{};                 ///< Owning copied/moved typed allowlist and factory captures.
  TextResolver resolver{};                          ///< Existing owning synchronous i18n adapter, used only at prepare.
  SceneResourceLimits limits{};                     ///< Finite parser bounds, independent of managed/tree limits.
  detail::ResourceRegistrationState registration{}; ///< Internal sticky failure; not a configuration/reset API.
  SceneResourceFormat format{
    SceneResourceFormat::LEGACY};           ///< Explicit grammar; ordinary resources retain their syntax.
  std::optional<ControlContext> controls{}; ///< Local immutable snapshot; absent inherits the containing scene context.

  /** @brief Register a typed resource control and retain the first failure for checked prepare/runApp.
   * @tparam T Public Widget subtype returned by the factory, including final types.
   * @tparam Factory Copyable callable accepting owning SceneResourceArguments and returning
   * Description<T> or exactly expected<Description<T>, ManagedError>.
   * @param name Case-sensitive ASCII identifier copied now, unique including builtin names.
   * @param factory Owning adapter copied/moved now and invoked at prepare after validation.
   * Borrowed captures must outlive prepare; no factory is invoked during registration.
   * @param children Whether this type accepts resource child nodes; defaults to a leaf.
   * @return This same configuration for chaining. The first failure stops later fluent registrations
   * and rejects prepare before file reads, parsing, resolvers, factories or terminal entry.
   * Internal allocation/capture exceptions become NO_MEMORY/CALLBACK_FAILED; caller argument
   * evaluation and construction/copying of the options or scene may still throw.
   */
  template <class T, class Factory>
    requires detail::ResourceFactory<T, std::decay_t<Factory>>
  SceneResourceOptions &control(std::string_view name, Factory &&factory, bool children = false) & noexcept {
    if (registration.failure_) {
      return *this;
    }

    try {
      const auto added = registry.add<T>(name, std::forward<Factory>(factory), children);

      if (!added) {
        registration.failure_ = std::pair{added.error(), TreeError::INVALID_ARGUMENT};
      }
    } catch (const std::bad_alloc &) {
      registration.failure_ = std::pair{SceneResourceErrorCode::NO_MEMORY, TreeError::NO_MEMORY};
    } catch (...) {
      registration.failure_ = std::pair{SceneResourceErrorCode::FACTORY, TreeError::CALLBACK_FAILED};
    }

    return *this;
  }

  /** @brief Select a finite native composite schema and collect registration failure for prepare/runApp.
   * @tparam T Factory widget result. @tparam Factory Existing owning resource callable.
   * @tparam Schema Exactly the canonical metadata span; no span is retained.
   * @param name Unique owning registry name. @param factory Real native MessageBox or Notifications recipe factory.
   * @param schema Complete canonical native composite vocabulary, checked immediately by content.
   * @return This configuration; the existing first-error and exception boundary is preserved.
   */
  template <class T, class Factory, class Schema>
    requires detail::ResourceFactory<T, std::decay_t<Factory>> &&
             std::same_as<std::remove_cvref_t<Schema>, std::span<const ControlPropertyDescriptor>>
  SceneResourceOptions &control(std::string_view name, Factory &&factory, Schema &&schema) & noexcept {
    if (registration.failure_) {
      return *this;
    }

    try {
      const auto added = registry.add<T>(name, std::forward<Factory>(factory), std::forward<Schema>(schema));

      if (!added) {
        registration.failure_ = std::pair{added.error(), TreeError::INVALID_ARGUMENT};
      }
    } catch (const std::bad_alloc &) {
      registration.failure_ = std::pair{SceneResourceErrorCode::NO_MEMORY, TreeError::NO_MEMORY};
    } catch (...) {
      registration.failure_ = std::pair{SceneResourceErrorCode::FACTORY, TreeError::CALLBACK_FAILED};
    }

    return *this;
  }

  /** @brief Select the finite native composite schema on a temporary without adapter copies.
   * @tparam T Factory widget result. @tparam Factory Existing resource callable. @tparam Schema Canonical span.
   * @param name Unique owning name. @param factory Owning actual native composite recipe producer.
   * @param schema Complete metadata read only now. @return This rvalue, with the same sticky error boundary.
   */
  template <class T, class Factory, class Schema>
    requires detail::ResourceFactory<T, std::decay_t<Factory>> &&
             std::same_as<std::remove_cvref_t<Schema>, std::span<const ControlPropertyDescriptor>>
  SceneResourceOptions &&control(std::string_view name, Factory &&factory, Schema &&schema) && noexcept {
    control<T>(name, std::forward<Factory>(factory), std::forward<Schema>(schema));

    return std::move(*this);
  }

  /** @brief Register on a temporary without copying its existing adapters.
   * @tparam T Public Widget subtype returned by the factory.
   * @tparam Factory Copyable typed recipe factory, as in the lvalue overload.
   * @param name Unique case-sensitive ASCII identifier copied now.
   * @param factory Owning callable transferred into the registry; borrows outlive prepare.
   * @param children Whether resource child nodes are allowed.
   * @return This configuration as an rvalue, with the same sticky checked-error contract.
   */
  template <class T, class Factory>
    requires detail::ResourceFactory<T, std::decay_t<Factory>>
  SceneResourceOptions &&control(std::string_view name, Factory &&factory, bool children = false) && noexcept {
    control<T>(name, std::forward<Factory>(factory), children);

    return std::move(*this);
  }
};

/** @brief Explicit owning file source; ordinary strings always denote source text. */
struct SceneResourceFile {
  std::filesystem::path path; ///< File read once at prepare; relative paths use the then-current working directory.
};

/** @brief Copy resource text into an existing consuming Description recipe.
 * @param source UTF-8 bytes copied now, parsed only when the recipe mounts.
 * @param options Owning typed registry, resolver and finite resource budgets.
 * @return Lazy Description sharing its parent's existing mount transaction and rollback.
 * Use inside a scene factory to capture newly created managed models in custom factories.
 * Allocation during recipe construction may throw; checked mount retains typed resource errors.
 */
Description<Widget> resource(std::string_view source, SceneResourceOptions options = {});

/** @brief Own an explicit file source as an ordinary consuming Description recipe.
 * @param source Owning path read once when the recipe mounts, before terminal open in a lazy Scene.
 * @param options Owning allowlist, resolver and finite parser budgets.
 * @return Lazy recipe sharing the enclosing transaction; file/parser failures create no resource nodes.
 * Construction may throw on allocation. No file watching or additional lifecycle is introduced.
 */
Description<Widget> resource(SceneResourceFile source, SceneResourceOptions options = {});

/** @brief Copy a resource string into the existing lazy scene facade.
 * @param source UTF-8 resource bytes copied now; no source view survives this call.
 * @param resource Owning registry, resolver and parser budgets consumed by the lazy recipe.
 * @param options Existing application/managed/facade policies, unchanged.
 * @param origin Construction site retained in checked diagnostics.
 * @return Lazy UI-affine scene; parse/schema/text/factory failures are ManagedError at prepare,
 * before opening a terminal. Construction/capture allocation may throw. Syntax is documented
 * in the scene-loading guide; this API executes no C++, PSS or arbitrary expressions.
 */
Scene scene(std::string_view source, SceneResourceOptions resource = {}, SceneOptions options = {},
            std::source_location origin = std::source_location::current());

/** @brief Read a bounded UTF-8 resource file at the existing prepare boundary.
 * @param source Owning explicit filesystem source, read once without changing the file.
 * @param resource Owning registry, resolver and finite budgets.
 * @param options Existing scene policies.
 * @param origin Construction site retained in checked diagnostics.
 * @return Lazy facade; open/read/parser failures retain typed resource position and OS error
 * before terminal open. Construction/capture allocation may throw; file bytes never escape as views.
 */
Scene scene(SceneResourceFile source, SceneResourceOptions resource = {}, SceneOptions options = {},
            std::source_location origin = std::source_location::current());
} // namespace portals::ui
