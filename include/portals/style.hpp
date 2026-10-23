// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/style_types.hpp>
#include <portals/widget.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace portals {
struct PssDocument;

namespace detail {
struct StyleThemeReceiver;
}

/** @brief Presentation states; ANY matches every state without changing behavior. */
enum class StyleState {
  ANY,      ///< Match all presentation variants.
  NORMAL,   ///< Ordinary content.
  FOCUSED,  ///< Synchronized control focus.
  DISABLED, ///< Synchronized control ineligibility.
  SELECTED, ///< Selected row or editor range.
  HEADER,   ///< Fixed table heading.
  CARET     ///< Editor software caret.
};

/** @brief Maximum owned ancestor names per style target, excluding its immediate type. */
inline constexpr std::size_t MAX_STYLE_BASE_TYPES = 32;

/** @brief Owned subject metadata, supplied explicitly for built-in or custom widget types. */
struct StyleTarget {
  /** @brief Destroy owned values. Deliberately suppress implicit noexcept moves: checked MSVC
   * container moves can allocate; copy-based transfers allow allocation failure to propagate.
   */
  ~StyleTarget() = default;

  std::string type{""}; ///< Case-sensitive type name, for example Button or Table.
  std::string id{""};   ///< Optional case-sensitive application identity, independent of NodeId.
  std::vector<std::string> classes =
    std::vector<std::string>(0); ///< Case-sensitive class names; all selector classes must match.
  bool control{false}; ///< Also match Control selectors; explicit targets opt in independently of their type name.
  std::vector<std::string> baseTypes = std::vector<std::string>(
    0); ///< Nearest-base-first owning names, at most MAX_STYLE_BASE_TYPES; no empty, wildcard, duplicate or self names.
};

/** @brief Owned metadata for one checked target in an atomic binding batch. */
struct StyleRegistration {
  Node<Widget> node;  ///< Observing handle; the tree retains widget ownership.
  StyleTarget target; ///< Matching metadata copied into the engine on success.
};

/** @brief Conjunction of optional type, id, classes and one state; no hierarchy combinators. */
struct StyleSelector {
  /** @brief Destroy owned values. Deliberately suppress implicit noexcept moves: checked MSVC
   * container moves can allocate; copy-based transfers allow allocation failure to propagate.
   */
  ~StyleSelector() = default;

  std::string type{""};                                           ///< Empty or * means any type.
  std::string id{""};                                             ///< Empty means any identity.
  std::vector<std::string> classes = std::vector<std::string>(0); ///< Every named class is required.
  StyleState state{StyleState::ANY};                              ///< Optional presentation-state predicate.
  /** @param other Owning value to compare.
   * @return Whether all typed fields are equal.
   */
  bool operator==(const StyleSelector &other) const = default;
};

/** @brief Sparse paint attributes for an existing presentation part. */
struct StylePaintDeclarations {
  std::optional<RgbColor> foreground; ///< RGB text color.
  std::optional<RgbColor> background; ///< RGB fill color.
  std::optional<bool> bold;           ///< Explicit bold request.
  std::optional<bool> italic;         ///< Explicit italic request.
  std::optional<bool> underline;      ///< Explicit underline request.

  /** @param other Value to compare. @return Whether all attributes agree. */
  bool operator==(const StylePaintDeclarations &other) const = default;
};

/** @brief Typed declarations; absent values preserve less-specific rules or widget defaults. */
struct StyleDeclarations {
  std::optional<RgbColor> foreground;       ///< RGB text color.
  std::optional<RgbColor> background;       ///< RGB fill color.
  std::optional<bool> bold;                 ///< Explicit true/false bold request.
  std::optional<bool> italic;               ///< Explicit true/false italic request.
  std::optional<bool> underline;            ///< Explicit true/false underline request.
  std::optional<std::int32_t> minWidth;     ///< Nonnegative cell minimum; allowed only on state-independent selectors.
  StylePaintDeclarations frameLine{};       ///< Frame perimeter overrides, inheriting the common resolved state.
  StylePaintDeclarations frameTitle{};      ///< Frame title text overrides.
  StylePaintDeclarations frameEnds{};       ///< Frame title prefix/suffix overrides.
  StylePaintDeclarations scrollbarTrack{};  ///< Scrollbar track overrides.
  StylePaintDeclarations scrollbarThumb{};  ///< Scrollbar thumb overrides.
  StylePaintDeclarations scrollbarEnds{};   ///< Scrollbar ends overrides.
  StylePaintDeclarations scrollbarCorner{}; ///< Scrollbar corner overrides.
  /** @param other Owning value to compare.
   * @return Whether all typed fields are equal.
   */
  bool operator==(const StyleDeclarations &other) const = default;
};

/** @brief One owned selector/declaration pair in source order. */
struct StyleRule {
  /** @brief Destroy owned values. Deliberately suppress implicit noexcept moves: checked MSVC
   * container moves can allocate; copy-based transfers allow allocation failure to propagate.
   */
  ~StyleRule() = default;

  StyleSelector selector;         ///< Matching and specificity criteria.
  StyleDeclarations declarations; ///< Values overriding less-specific rules.
  /** @param other Owning value to compare.
   * @return Whether all typed fields are equal.
   */
  bool operator==(const StyleRule &other) const = default;
};

/** @brief Owned immutable-at-use rules; constructing typed values may allocate and throw. */
struct StyleSheet {
  /** @brief Destroy owned values. Deliberately suppress implicit noexcept moves: checked MSVC
   * container moves can allocate; copy-based transfers allow allocation failure to propagate.
   */
  ~StyleSheet() = default;

  std::vector<StyleRule> rules =
    std::vector<StyleRule>(0); ///< Source-order rules, at most MAX_STYLE_RULES when installed.
  std::optional<NodeId>
    scope{}; ///< Optional exact node restriction; preserves selector specificity and source metadata.
  /** @param other Owning value to compare.
   * @return Whether all typed fields are equal.
   */
  bool operator==(const StyleSheet &other) const = default;
};

/** @brief Independent cascade priority, before selector specificity. */
enum class StyleTier {
  DEFAULTS,    ///< Declarations above widget defaults and below all other tiers.
  THEME,       ///< Reserved theme declarations controlled by one authority lease.
  APPLICATION, ///< Application overrides, above THEME.
  USER         ///< User overrides, above APPLICATION.
};

/** @brief Aggregate logical owning payload budgets; base slots count as layers. */
struct StyleLayerOptions {
  std::size_t maxLayers{16};              ///< Resident layer slots, including the two permanent bases.
  std::size_t maxRules{1024};             ///< Rules summed across the complete resident table.
  std::size_t maxDeclarations{6144};      ///< Present optional fields, including explicit false values.
  std::size_t maxOwnedBytes{1024 * 1024}; ///< Logical payload charge; not a whole-heap allocator cap.
  /** @param other Owning value to compare.
   * @return Whether all typed fields are equal.
   */
  bool operator==(const StyleLayerOptions &other) const = default;
};

/** @brief Weak UI-affine observer; destruction does not remove its layer. */
class StyleLayerHandle {
public:
  /** @brief Construct an empty observer; layer mutations reject it as INVALID_ARGUMENT. */
  StyleLayerHandle() = default;

private:
  friend class StyleEngine;
  std::weak_ptr<const void> lifetime_;
  std::thread::id owner_;
  std::uint64_t epoch_{};
  std::uint64_t id_{};
};

/** @brief Replace an existing layer, or remove it when sheet is absent. */
struct StyleLayerUpdate {
  StyleLayerHandle handle;         ///< Existing layer identity; duplicate identities reject the batch.
  std::optional<StyleSheet> sheet; ///< Owning replacement or nullopt for removal.
};

namespace detail {
/** @brief Private captured theme authority, including legacy epoch admission. */
struct StyleThemeLease {
  bool layered{};        ///< Captured opt-in mode; controllers never silently adopt a new mode.
  std::uint64_t epoch{}; ///< Engine identity generation captured at attach.
  std::uint64_t id{};    ///< Permanent THEME slot identity, zero in legacy mode.
  std::uint64_t owner{}; ///< Reserved THEME authority generation.
};
} // namespace detail

/** @brief Finite typed/PSS rule budget. */
inline constexpr std::size_t MAX_STYLE_RULES = 1024;
/** @brief Finite parser input byte budget. */
inline constexpr std::size_t MAX_PSS_BYTES = 1024 * 1024;

/** @brief Allocation-free error; message borrows a static string literal. */
struct StyleError {
  TreeError cause{TreeError::INVALID_ARGUMENT}; ///< Tree/validation/allocation failure category.
  std::size_t line{1};                          ///< One-based PSS line, or one for typed/tree errors.
  std::size_t column{1};                        ///< One-based byte column, or one for typed/tree errors.
  std::string_view message;                     ///< Static diagnostic; never borrows source input.
};

/** @brief Validate bounded ancestor metadata without allocation or retaining input.
 * @param target Stable owning metadata; a nonempty lineage requires a concrete immediate type.
 * @return Success or INVALID_ARGUMENT for excessive depth, empty/wildcard, duplicate or self ancestors.
 */
std::expected<void, StyleError> validateStyleTarget(const StyleTarget &target) noexcept;

/** @brief Parse strict PSS v0 without I/O or mutation, on any thread.
 * @param source Borrowed UTF-8 source; identifiers and grammar are ASCII, copied on success.
 * @return Complete owned sheet or precise error; no partial sheet escapes. Allocation failure is NO_MEMORY.
 */
std::expected<StyleSheet, StyleError> parsePss(std::string_view source);

/** @brief Resolve one subject through the same engine used by typed and PSS installation.
 * @param sheet Valid rules (nonnegative minWidth, at most MAX_STYLE_RULES); immutable during this call.
 * This metadata-only preview ignores a sheet node scope; the owning engine checks scope against live NodeId.
 * @param target Borrowed matching metadata; no data are retained.
 * @param defaults Owning built-in/custom widget defaults beneath all matching declarations.
 * @return Complete owning style, or unchanged defaults for invalid ancestor metadata.
 * No allocation, I/O or thread affinity; caller owns input stability. validateStyleTarget provides typed diagnostics.
 */
ComputedStyle resolveStyle(const StyleSheet &sheet, const StyleTarget &target, ComputedStyle defaults = {}) noexcept;

/** @brief Typed palette tokens for the built-in theme factories. */
struct ThemeTokens {
  RgbColor foreground; ///< Normal foreground.
  RgbColor background; ///< Normal background.
  RgbColor accent;     ///< Focus background and contrasting caret foreground.
  RgbColor muted;      ///< Disabled foreground.
  RgbColor selection;  ///< Selected-row/range background.
  RgbColor heading;    ///< Heading foreground.
};

/** @param tokens Owning palette colors.
 * @return Typed theme rules; allocation failure throws before tree mutation.
 */
StyleSheet themeFromTokens(ThemeTokens tokens);
/** @return Typed Modern theme, equivalent to themes/modern.pss; may allocate. */
StyleSheet modernTheme();
/** @return Typed Turbo-like theme, equivalent to themes/turbo.pss; may allocate. */
StyleSheet turboTheme();

/** @return Typed Nord palette adapted from Textual through ThemeTokens; may allocate. */
StyleSheet nordTheme();

/** @return Typed Gruvbox palette adapted from Textual through ThemeTokens; may allocate. */
StyleSheet gruvboxTheme();

/** @return Typed Tokyo Night palette adapted from Textual through ThemeTokens; may allocate. */
StyleSheet tokyoNightTheme();

/** @return Typed light Catppuccin Latte palette adapted from Textual through ThemeTokens; may allocate. */
StyleSheet catppuccinLatteTheme();

/** @brief UI-owned style transaction coordinator; tree must outlive this nonmovable object.
 * Bindings own metadata and only observe Node handles. Stale bindings are pruned on the next
 * successful operation. Destruction leaves already computed widget values intact. One engine
 * may own a widget's style at a time; overlapping engines are unsupported. No notifications, I/O,
 * workers or hot reload. All mutations reject active frames and wrong-thread calls before work.
 */
class StyleEngine {
public:
  /** @param tree Borrowed UI owner, required to outlive the engine.
   * @param maxBindings Positive registry budget; default matches the tree's default capacity.
   */
  explicit StyleEngine(WidgetTree &tree, std::size_t maxBindings = 4096);
  StyleEngine(const StyleEngine &) = delete;
  StyleEngine &operator=(const StyleEngine &) = delete;

  /** @brief Register/update metadata and atomically recompute every bound widget.
   * @param node Checked target; no ownership transfer.
   * @param target Owned matching metadata, copied during staging.
   * @return Coalesced invalidation, or error retaining every previous value and binding.
   */
  std::expected<Invalidation, StyleError> bind(Node<Widget> node, StyleTarget target);

  /// @brief Update metadata in one transaction, then recompute every bound widget once.
  /// Duplicate input nodes return INVALID_ARGUMENT. Stale bindings are pruned before admission.
  /// Empty input performs the same refresh as refresh().
  /// With index allocation available, identity lookup takes O((B + R) log(R + 2))
  /// work and O(R) temporary space
  /// for B existing bindings and R input registrations. Metadata copies and style
  /// resolution are additional costs; existing callback order is preserved.
  /// If index allocation fails, an allocation-free O(R^2) validation preserves
  /// argument-error priority before returning NO_MEMORY for an otherwise valid batch.
  /// @param registrations Borrowed live targets and metadata, copied during staging.
  /// @return Coalesced invalidation, or an error preserving all values, bindings and revisions.
  std::expected<Invalidation, StyleError> bindBatch(std::span<const StyleRegistration> registrations);
  /** @brief Install a complete typed stylesheet atomically between frames.
   * @param sheet Owned replacement rules; validation and staging precede commit.
   * @return Actual paint/layout invalidation, or error retaining sheet/revision/widget values.
   */
  std::expected<Invalidation, StyleError> replace(StyleSheet sheet);
  /** @brief Parse and install without ever exposing a partial stylesheet.
   * @param source Borrowed PSS; copied by parsing before the atomic commit.
   * @return Actual invalidation or source/tree error retaining the prior revision.
   */
  std::expected<Invalidation, StyleError> replacePss(std::string_view source);
  /** @brief Recompute against current manual widget options, between frames.
   * @return Actual invalidation or error with no partial update.
   */
  std::expected<Invalidation, StyleError> refresh();
  /** @brief Inspect on UI; no allocation or tree access.
   * @return Last committed revision, initially zero; equal styles can still advance it.
   */
  std::uint64_t revision() const noexcept;

  /** @brief Enable owning tiers atomically; equal configuration is a no-op.
   * @param options Nonzero aggregate limits, including at least two layer slots.
   * @return Effective invalidation or atomic boundary/budget/identity/allocation failure.
   */
  std::expected<Invalidation, StyleError> enableLayers(StyleLayerOptions options = {});
  /** @brief Install an owning APPLICATION or USER layer with a unique tier name.
   * @param tier APPLICATION or USER priority.
   * @param name Nonempty tier-unique name, normalized into ownership after budget admission.
   * @param sheet Borrowed declarations, copied after admission; input remains caller-owned.
   * @return Weak observer or atomic failure; destroying the observer leaves declarations installed.
   */
  std::expected<StyleLayerHandle, StyleError> addLayer(StyleTier tier, std::string_view name, const StyleSheet &sheet);
  /** @brief Replace a non-base layer while retaining its installation order.
   * @param handle Existing non-base observer, admitted on the engine UI thread.
   * @param sheet Borrowed candidate; equal values allocate nothing and retain revision.
   * @return Actual invalidation or an error retaining all committed state.
   */
  std::expected<Invalidation, StyleError> replaceLayer(StyleLayerHandle handle, const StyleSheet &sheet);
  /** @brief Remove declarations; base slots remain and revoke their owner.
   * @param handle Live observer; removed non-base identities cannot be reused.
   * @return Actual invalidation or an error retaining all committed state.
   */
  std::expected<Invalidation, StyleError> removeLayer(StyleLayerHandle handle);
  /** @brief Atomically update existing unique handles. Empty input is a no-op.
   * @param updates Borrowed complete batch; duplicate handles and base replacements are rejected.
   * @return Actual invalidation or an error retaining all declarations, bindings and identities.
   */
  std::expected<Invalidation, StyleError> updateLayers(std::span<const StyleLayerUpdate> updates);
  /** @brief Observe one permanent base slot on its UI thread.
   * @param tier DEFAULTS or THEME priority.
   * @return Weak observer or boundary/argument error; allocation-free.
   */
  std::expected<StyleLayerHandle, StyleError> baseLayer(StyleTier tier) const;
  /** @brief Replace the DEFAULTS slot beneath all other layers.
   * @param sheet Borrowed replacement declarations, copied only after budget admission.
   * @return Actual invalidation or atomic failure; layers must already be enabled.
   */
  std::expected<Invalidation, StyleError> defaults(const StyleSheet &sheet);
  /** @brief Replace THEME and hand authority to the caller, including equal values.
   * @param sheet Borrowed replacement, installed as the whole sheet in legacy mode.
   * @return Actual invalidation or atomic failure; failed changes never revoke prior authority.
   */
  std::expected<Invalidation, StyleError> theme(const StyleSheet &sheet);

private:
  friend class ManagedScene;
  friend struct StyleLayerTestAccess;

  struct Layer {
    Layer(std::uint64_t identity = 0, std::uint64_t installationOrder = 0, StyleTier priority = StyleTier::DEFAULTS)
        : id(identity), order(installationOrder), tier(priority), name("") {
    }

    // Checked MSVC string/container moves can allocate; transfer records by copy or swap.
    ~Layer() = default;
    std::uint64_t id;
    std::uint64_t order;
    StyleTier tier;
    std::string name;
    StyleSheet sheet;
    std::shared_ptr<const PssDocument> document;
  };

  struct LayerState {
    std::vector<Layer> layers = std::vector<Layer>(0);
    StyleLayerOptions options;
    std::uint64_t epoch{}; ///< Engine identity generation captured at attach.
    std::uint64_t nextId{1};
    std::uint64_t nextOrder{1};
    std::uint64_t owner{};     ///< Reserved THEME authority generation.
    std::uint64_t themeBase{}; ///< Permanent THEME identity, distinct from its temporary masked contribution.
    std::uint64_t preview{};   ///< Private effective THEME contribution; zero exposes the permanent base.
    bool enabled{};
  };
  enum class Mutation {
    ENABLE,
    ADD,
    UPDATE,
    DEFAULTS,
    THEME,
    OWNED_THEME,
    ADMIT,
    PREVIEW,
    PREVIEW_COMMIT,
    PREVIEW_CANCEL
  };

  struct PreviewCapture {
    std::uint64_t revision{};
    detail::StyleThemeLease lease;
    std::weak_ptr<const void> lifetime;
  };

  struct Binding {
    Node<Widget> node;
    StyleTarget target;
  };

  std::expected<Invalidation, StyleError> install(StyleSheet sheet, std::vector<Binding> &&bindings,
                                                  LayerState *candidate = nullptr, bool wholeReplace = false,
                                                  const detail::StyleThemeLease *lease = nullptr,
                                                  const PreviewCapture *previewCapture = nullptr,
                                                  bool notifyTheme = false);
  std::expected<Invalidation, StyleError> replaceOwnedTheme(const StyleSheet &sheet, detail::StyleThemeLease lease);
  std::expected<void, StyleError> mergeBindings(std::span<const StyleRegistration> registrations,
                                                std::optional<std::vector<Binding>> &destination);
  std::expected<Invalidation, StyleError>
  mutate(Mutation mutation, StyleLayerOptions options, StyleTier tier, std::string_view name, const StyleSheet *sheet,
         std::span<const StyleLayerUpdate> updates, std::span<const StyleRegistration> registrations,
         StyleLayerHandle *added = nullptr, const detail::StyleThemeLease *lease = nullptr,
         const PssDocument *document = nullptr, std::optional<StyleLayerOptions> admissionOptions = std::nullopt,
         const PreviewCapture *previewCapture = nullptr);
  PreviewCapture capturePreview() const noexcept;
  std::expected<void, StyleError> validatePreviewCapture(const PreviewCapture &capture) const;
  bool ownsPreview(const StyleLayerHandle &handle) const noexcept;
  std::expected<void, StyleError> validateHandle(const StyleLayerHandle &handle) const;
  std::expected<void, StyleError> validateLease(detail::StyleThemeLease lease) const;
  detail::StyleThemeLease themeLease() const noexcept;
  StyleLayerHandle handle(std::uint64_t id, std::uint64_t epoch) const noexcept;
  ComputedStyle resolve(const StyleSheet &sheet, const LayerState &state, const StyleTarget &target,
                        ComputedStyle defaults, NodeId node = {}) const noexcept;
  std::expected<void, StyleError> checkBoundary(bool completing = false) const;
  WidgetTree &tree_;
  std::weak_ptr<detail::StyleThemeReceiver> themeReceiver_;
  std::size_t maxBindings_;
  StyleSheet sheet_;
  std::vector<Binding> bindings_ = std::vector<Binding>(0);
  std::uint64_t revision_{};
  std::shared_ptr<const void> lifetime_ = std::make_shared<const char>(char{});
  const std::thread::id owner_ = std::this_thread::get_id();
  LayerState layerState_;
  bool stagingLayers_{};
};
} // namespace portals
