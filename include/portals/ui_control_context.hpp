// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/line_edit.hpp>
#include <portals/text_value.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace portals::ui {

/** @brief Owning source coordinates; offsets and columns are UTF-8 bytes, lines/columns are one-based. */
struct ControlSourceOrigin {
  ~ControlSourceOrigin() = default;

  std::string sourceIdentity = std::string(0, '\0'); ///< Caller-defined file/resource identity.
  std::size_t byteOffset{};                          ///< Zero-based byte offset in that source.
  std::size_t line{1};                               ///< One-based source line.
  std::size_t column{1};                             ///< One-based byte column.
  std::shared_ptr<const std::string>
    sharedSourceIdentity{}; ///< Shared identity, preferred over sourceIdentity when present.
};

/** @brief Provenance of one sparse property; shared storage is immutable and owned. */
struct ControlPropertyOrigin {
  ~ControlPropertyOrigin() = default;

  std::string path = std::string(0, '\0');             ///< Canonical relative path, e.g. history.maxBytes.
  std::shared_ptr<const ControlSourceOrigin> source{}; ///< Original declaration location.
};

/** @brief Sparse editor history settings; explicit false and zero remain present. */
struct ControlHistorySettings {
  std::optional<bool> enabled{};              ///< Whether this instance retains undo history.
  std::optional<std::size_t> maxOperations{}; ///< Maximum retained operations; zero disables retention.
  std::optional<std::size_t> maxBytes{};      ///< Maximum retained payload bytes; zero disables retention.
  std::optional<bool> groupTyping{};          ///< Group adjacent ordinary typing operations.
  std::optional<bool> groupDeletion{};        ///< Group adjacent ordinary deletion operations.
};

/** @brief Owning sparse literals only; never contains IDs, models, handlers or live history. */
struct ControlSettings {
  ~ControlSettings() = default;

  std::optional<TextValue> text{};                        ///< Caption/value expression, resolved by receiving scene.
  std::optional<std::int32_t> width{};                    ///< Nonnegative fixed width in cells.
  std::optional<std::int32_t> height{};                   ///< Nonnegative fixed height in cells.
  std::optional<std::uint32_t> flex{};                    ///< Positive common flex weight.
  std::optional<std::int32_t> gap{};                      ///< Nonnegative VBox/HBox gap.
  std::optional<CheckState> state{};                      ///< CheckBox/ThreeState initial state.
  std::optional<CheckCycle> cycle{};                      ///< CheckBox cycle; ThreeState is fixed THREE_STATE.
  std::optional<EditMode> editMode{};                     ///< LineEdit publication mode.
  std::optional<bool> readOnly{};                         ///< LineEdit writable policy.
  std::optional<ProgrammaticChange> programmaticChange{}; ///< LineEdit initial programmatic history policy.
  std::optional<LineEditDisplayMode> displayMode{};       ///< LineEdit immutable presentation.
  std::optional<TextValue> placeholder{};                 ///< LineEdit placeholder expression.
  ControlHistorySettings history{};                       ///< Structural merge, never a whole-history reset.
  std::shared_ptr<const ControlSourceOrigin> origin{};    ///< Fallback location for present properties.
  std::vector<ControlPropertyOrigin> origins = std::vector<ControlPropertyOrigin>(0); ///< Per-property overrides.
};

/** @brief Finite context bounds, checked before publication/materialization beyond each bound. */
struct ControlContextLimits {
  std::size_t maxDefinitions{64};           ///< User definitions; builtin types do not consume this budget.
  std::size_t maxInheritance{32};           ///< Ancestor identities, excluding the requested type; at most 32.
  std::size_t maxStructDepth{8};            ///< Property depth: scalar one, history member two.
  std::size_t maxMaterializedLeaves{65536}; ///< Leaves per resolve; mount must also aggregate across all nodes.
};

/** @brief Configuration-only failure, before resolvers, factories or mounting. */
enum class ControlContextErrorCode {
  NO_MEMORY,               ///< Allocation failed; diagnostic storage may be absent.
  CONFIGURATION_FAILED,    ///< Unexpected internal preparation exception.
  INVALID_LIMIT,           ///< At least one finite bound is zero.
  INVALID_NAME,            ///< Type name is not an ASCII identifier.
  DUPLICATE_TYPE,          ///< A type definition/schema already exists.
  BUILTIN_SHADOW,          ///< A user type attempts to replace a builtin.
  UNKNOWN_TYPE,            ///< A requested type or definition base is missing.
  INHERITANCE_CYCLE,       ///< Definition bases contain a cycle.
  INHERITANCE_LIMIT,       ///< Effective ancestor count exceeds its bound.
  DEFINITION_LIMIT,        ///< User definitions and custom schemas exceed their shared bound.
  STRUCT_DEPTH_LIMIT,      ///< A present property's structural depth exceeds its bound.
  MATERIALIZED_LEAF_LIMIT, ///< Effective sparse scalar leaf count exceeds its bound.
  UNKNOWN_PROPERTY,        ///< A schema/origin path is outside the canonical vocabulary.
  DUPLICATE_PROPERTY,      ///< Duplicate defaults block, schema path or property provenance.
  UNSUPPORTED_PROPERTY,    ///< Known property is not supported by the concrete base.
  INVALID_VALUE,           ///< Scalar range or enum is invalid.
  FIXED_CYCLE              ///< ThreeState lineage attempts a cycle other than THREE_STATE.
};

/** @brief Independently owned diagnostic; copying errors never copies strings. */
struct ControlContextDiagnostic {
  ~ControlContextDiagnostic() = default;

  std::string path = std::string(0, '\0');             ///< Fully qualified type.property, or offending type.
  std::shared_ptr<const ControlSourceOrigin> origin{}; ///< Original inherited declaration, when supplied.
};

/** @brief Allocation-free typed fallback remains usable if diagnostic construction fails. */
struct ControlContextError {
  ControlContextErrorCode code{ControlContextErrorCode::CONFIGURATION_FAILED}; ///< Typed failure.
  std::shared_ptr<const ControlContextDiagnostic> diagnostic{};                ///< Optional owning details.
};

/** @brief Scalar schema kind, shared by C++ validation and resource decoding. */
enum class ControlPropertyKind {
  TEXT,             ///< Owning TextValue expression.
  BOOLEAN,          ///< Explicit true or false.
  SIGNED_INTEGER,   ///< Signed cell extent.
  UNSIGNED_INTEGER, ///< Nonnegative capacity or positive weight.
  ENUMERATION       ///< One of the descriptor's symbolic enum names.
};

/** @brief Static canonical property descriptor; returned views have process lifetime. */
struct ControlPropertyDescriptor {
  std::string_view path;                       ///< Canonical scalar path, including nested history members.
  ControlPropertyKind kind;                    ///< Literal kind; enum names follow the existing public enumerators.
  std::size_t depth;                           ///< Structural path depth.
  std::span<const std::string_view> enumNames; ///< Allowed symbolic enum names, empty for other kinds.
};

/** @brief Return the canonical schema for a builtin concrete type; unknown types have an empty schema.
 * @param builtin One of VBox/HBox/Label/Button/CheckBox/LineEdit/ThreeState.
 * @return Static descriptors only; named definitions are resolved to their concrete base first.
 */
std::span<const ControlPropertyDescriptor> controlPropertySchema(std::string_view builtin) noexcept;

/** @brief Owning effective sparse configuration and type identity, with no scene-affine state. */
struct ResolvedControl {
  ~ResolvedControl() = default;

  std::string type = std::string(0, '\0'); ///< Immediate requested named/builtin type.
  std::string base = std::string(0, '\0'); ///< Concrete builtin factory type, preserving ThreeState.
  std::vector<std::string> lineage = std::vector<std::string>(0); ///< Immediate-to-distant ancestors; excludes type.
  ControlSettings settings;                                       ///< Merged sparse settings with winning origins.
  std::size_t materializedLeaves{}; ///< Present scalar leaves; caller aggregates per mount.
};

/** @brief Cheap owning immutable snapshot of named definitions and defaults.
 * Const snapshot reads are concurrent-safe. Mutating the same handle requires external synchronization.
 * Copies preserve the snapshot and sticky error; later fluent mutations publish a new version only.
 * Construction/copy of caller arguments may throw; fluent operations catch internal allocation failures.
 * No callback, resolver, factory, locale, runtime or mutable cache is retained or invoked.
 */
class ControlContext {
public:
  /** @param limits Positive finite limits; invalid limits become a sticky configuration error. */
  explicit ControlContext(ControlContextLimits limits = {}) noexcept;

  /** @brief Register a callback-free custom factory schema from canonical scalar property paths.
   * @param name Unique non-builtin type identity, shared with the caller's explicit factory allowlist.
   * @param properties Allowed paths from the builtin descriptor vocabulary; copied now, no borrowed span survives.
   * @return This same handle; custom types and named definitions share maxDefinitions. Unknown paths fail.
   */
  ControlContext &registerType(std::string_view name, std::span<const std::string_view> properties) & noexcept;

  /** @param name Custom schema name. @param properties Copied canonical paths. @return This temporary handle. */
  ControlContext &&registerType(std::string_view name, std::span<const std::string_view> properties) && noexcept {
    registerType(name, properties);

    return std::move(*this);
  }

  /** @brief Add one unique named definition; base may be declared later.
   * @param name Case-sensitive ASCII identifier, excluding builtin names.
   * @param base One builtin or named definition; checked by validate/resolve.
   * @param settings Owning sparse initial settings copied during this call.
   * @return This same handle. The first typed failure stops later fluent mutations.
   */
  ControlContext &define(std::string_view name, std::string_view base, const ControlSettings &settings = {}) & noexcept;

  /** @param name Variant name. @param base Ancestor identity. @param settings Sparse patch. @return This temporary. */
  ControlContext &&define(std::string_view name, std::string_view base,
                          const ControlSettings &settings = {}) && noexcept {
    define(name, base, settings);

    return std::move(*this);
  }

  /** @brief Register the sole defaults block for one builtin/named type.
   * @param type Case-sensitive type identity; forward definitions are permitted.
   * @param settings Sparse patch; a second defaults block for this type is a typed duplicate.
   * @return This same handle; internal exceptions become a sticky typed error.
   */
  ControlContext &defaults(std::string_view type, const ControlSettings &settings) & noexcept;

  /** @param type Defaults target. @param settings Sparse patch. @return This temporary handle. */
  ControlContext &&defaults(std::string_view type, const ControlSettings &settings) && noexcept {
    defaults(type, settings);

    return std::move(*this);
  }

  /** @brief Apply sparse defaults in a nested configuration scope without changing earlier snapshots.
   * @param type Existing or forward-declared type receiving local defaults.
   * @param settings Present leaves override inherited defaults, including false, zero and their provenance.
   * @return This handle with a new immutable version; a resource loader separately rejects duplicate local blocks.
   */
  ControlContext &overlayDefaults(std::string_view type, const ControlSettings &settings) & noexcept;

  /** @param type Local defaults target. @param settings Sparse patch. @return This temporary handle. */
  ControlContext &&overlayDefaults(std::string_view type, const ControlSettings &settings) && noexcept {
    overlayDefaults(type, settings);

    return std::move(*this);
  }

  /** @brief Validate every definition/default before any application effect.
   * @return First sticky, graph, schema or bounded-materialization failure, otherwise success.
   */
  std::expected<void, ControlContextError> validate() const noexcept;

  /** @brief Validate once and return an immutable version with reusable graph qualification.
   * @return Checked snapshot; resolve still validates each instance patch. Later fluent mutation
   * invalidates qualification only on the mutated handle, leaving earlier copies unchanged.
   */
  std::expected<ControlContext, ControlContextError> prepared() const noexcept;

  /** @return Whether this is the unconfigured context with ordinary default budgets. */
  bool empty() const noexcept;

  /** @param name Exact custom schema name, excluding builtins and named definitions.
   * @return Whether this snapshot contains the explicit callback-free schema.
   */
  bool hasRegisteredType(std::string_view name) const noexcept;

  /** @param name Exact custom schema name.
   * @param properties Canonical allowed paths, compared as an unordered set without allocation.
   * @return Exact schema equality; false for missing types, unknown paths or duplicate paths.
   */
  bool matchesRegisteredType(std::string_view name, std::span<const std::string_view> properties) const noexcept;

  /** @brief Validate the whole snapshot, then resolve builtin/defaults/definition/defaults/instance precedence.
   * @param name Requested builtin or named type.
   * @param instancePatch Final sparse overrides, copied without resolving TextValue expressions.
   * @return Owning effective sparse settings or a typed failure with original declaration provenance.
   */
  std::expected<ResolvedControl, ControlContextError> resolve(std::string_view name,
                                                              const ControlSettings &instancePatch = {}) const noexcept;

  /** @return Immutable bounds captured by this snapshot. */
  ControlContextLimits limits() const noexcept;

private:
  struct Data;
  std::shared_ptr<const Data> data_{};
  ControlContextLimits limits_;
  std::optional<ControlContextError> failure_{};
  bool prepared_{};
};

} // namespace portals::ui
