// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/style_pss.hpp>
#include <portals/ui.hpp>

#include <filesystem>

namespace portals::ui {
/** @brief Style source admission and existing layer policy. */
struct StyleOptions {
  StyleTier tier{StyleTier::APPLICATION};    ///< APPLICATION by default; USER must be explicit.
  std::optional<std::string> name{};         ///< Explicit tier/name upsert, or independent owning stage identity.
  std::string sourceName{""};                ///< Owning UTF-8 source label; empty preserves a document's identity.
  std::optional<StyleLayerOptions> layers{}; ///< Omitted adopts existing limits or atomic first-enable defaults.
};

/** @brief Owning explicit file source; no watcher, includes or implicit path detection. */
struct FileSource {
  /** @brief Suppress implicit noexcept path moves; checked MSVC path copies can allocate. */
  ~FileSource() = default;

  std::filesystem::path path; ///< Owned native path; Windows Unicode is retained without narrow conversion.
};

/** @param path Explicit native path copied into the source; construction may throw before consumption.
 * @return Owning file source, read only at admitted scene consumption.
 */
inline FileSource file(const std::filesystem::path &path) {
  return {path};
}

/** @brief Copyable immutable owning style payload with one shared stage identity.
 * Independent unnamed stages compose. Reusing a stage in one scene checks its original real layer handle,
 * preserves order and rejects removal as stale; named stages explicitly upsert tier/name.
 * Construction can throw before a receiving facade boundary. Copies retain no UI owner.
 */
class StyleStage {
public:
  /** @param other Owning immutable stage identity to share; moves use this same nonconsuming copy.
   * Stage transfer never creates an empty payload or invalidates the original stage.
   */
  StyleStage(const StyleStage &other) noexcept = default;

  /** @param other Shared immutable payload replacing this stage identity.
   * @return This stage after a nonallocating identity copy.
   */
  StyleStage &operator=(const StyleStage &other) noexcept = default;

  /** @param input Sized strict PSS input, copied/parsed before receiving facade entry.
   * @param options Existing layer admission/source policy.
   * @param origin Stage registration site.
   */
  StyleStage(std::string_view input, const StyleOptions &options = {},
             std::source_location origin = std::source_location::current())
      : StyleStage(bytes(input, options, origin)) {
  }

  /** @param input Owning string borrowed only during stage construction.
   * @param options Existing layer policy.
   * @param origin Stage registration site.
   */
  StyleStage(const std::string &input, const StyleOptions &options = {},
             std::source_location origin = std::source_location::current())
      : StyleStage(bytes(input, options, origin)) {
  }

  /** @tparam N Full char array extent.
   * @param input Full array; only a final terminator is excluded, embedded NUL remains invalid.
   * @param options Existing layer policy.
   * @param origin Stage registration site.
   */
  template <std::size_t N>
  StyleStage(const char (&input)[N], const StyleOptions &options = {},
             std::source_location origin = std::source_location::current())
      : StyleStage(bytes(std::string_view(input, N && input[N - 1] == '\0' ? N - 1 : N), options, origin)) {
  }

  /** @tparam Pointer Exactly char* or const char*, without accepting arrays through this overload.
   * @param input Valid nonnull terminated C string; use a sized view for arbitrary bytes.
   * @param options Existing layer policy.
   * @param origin Stage registration site.
   */
  template <class Pointer>
    requires(std::same_as<std::remove_cvref_t<Pointer>, const char *> ||
             std::same_as<std::remove_cvref_t<Pointer>, char *>)
  StyleStage(Pointer &&input, const StyleOptions &options = {},
             std::source_location origin = std::source_location::current())
      : StyleStage(bytes(std::string_view(input), options, origin)) {
  }

  /** @param input Mutable document admitted before an immutable deep copy.
   * @param options Existing layer/source policy.
   * @param origin Registration site.
   */
  StyleStage(const PssDocument &input, const StyleOptions &options = {},
             std::source_location origin = std::source_location::current())
      : StyleStage(document(input, options, origin)) {
  }

  /** @param input Explicit native owning file path.
   * @param options Existing layer/source policy.
   * @param origin Registration site.
   */
  StyleStage(const FileSource &input, const StyleOptions &options = {},
             std::source_location origin = std::source_location::current())
      : StyleStage(source(input, options, origin)) {
  }

  /** @tparam R Checked rule capacity.
   * @tparam C Checked class capacity.
   * @tparam D Checked occurrence capacity.
   * @tparam B Checked owning text capacity.
   * @param input Existing checked compact sheet.
   * @param options Existing layer/source policy.
   * @param origin Registration site.
   */
  template <std::size_t R, std::size_t C, std::size_t D, std::size_t B>
  StyleStage(const portals::style::ct::CompactSheet<R, C, D, B> &input, const StyleOptions &options = {},
             std::source_location origin = std::source_location::current())
      : StyleStage(compact(input, options, origin)) {
  }

  /** @param bytes Strict UTF-8 PSS bytes, borrowed only during owning parse.
   * @param options Layer admission and owning source identity.
   * @param origin Stage construction site used by pipe registration.
   * @return Owning immutable parsed stage or owning fixed error stage; no borrowed bytes escape.
   * Error labels retain only a valid UTF-8 prefix within 4096 bytes, or empty for invalid UTF-8/NUL.
   */
  static StyleStage bytes(std::string_view bytes, const StyleOptions &options, std::source_location origin);

  /** @param document Mutable public document borrowed for allocation-free validation before copying.
   * @param options Layer admission; empty sourceName preserves document identity.
   * @param origin Stage construction site.
   * @return Owning immutable document or fixed error stage, never an alias of mutable caller storage.
   */
  static StyleStage document(const PssDocument &document, const StyleOptions &options, std::source_location origin);

  /** @param source Explicit owning file source, read at consumption.
   * @param options Layer admission/source label.
   * @param origin Stage construction site.
   * @return Owning deferred file stage.
   */
  static StyleStage source(const FileSource &source, const StyleOptions &options, std::source_location origin);

  /** @param failure Fixed owning parser/materialization error with static message.
   * @param options Layer policy and bounded owning UTF-8 source label; invalid labels become empty.
   * @param origin Stage construction site.
   * @return Deferred typed failure with no retained input view.
   */
  static StyleStage failure(PssError failure, const StyleOptions &options, std::source_location origin);

  /** @param owner Stationary receiving facade.
   * @param stage Shared immutable owning stage.
   * @return Same facade reference after one dispatch.
   */
  friend Scene &operator|(Scene &owner, const StyleStage &stage) {
    return owner.style(stage, stage.origin_);
  }

  /** @param input Consumed idle facade; owning move admission happens before stage dispatch.
   * @param stage Shared immutable stage.
   * @return Owning facade after one dispatch, safe for temporary chains.
   */
  friend Scene operator|(Scene &&input, const StyleStage &stage) {
    Scene owner(std::move(input));

    owner.style(stage, stage.origin_);

    return owner;
  }

private:
  friend class Scene;

  template <std::size_t R, std::size_t C, std::size_t D, std::size_t B>
  static StyleStage compact(const portals::style::ct::CompactSheet<R, C, D, B> &input, const StyleOptions &options,
                            std::source_location origin) {
    auto converted = portals::style::ct::materializeDocument(input);

    return converted ? document(*converted, options, origin) : failure(converted.error(), options, origin);
  }

  ManagedError boundaryFailure(TreeError cause) const;

  struct Impl;

  StyleStage(std::shared_ptr<const Impl> payload, std::source_location origin)
      : payload_(std::move(payload)), origin_(origin) {
  }

  std::expected<StyleLayerHandle, ManagedError> apply(ManagedScene &scene, const StyleLayerHandle *existing) const;
  std::shared_ptr<const Impl> payload_;
  std::source_location origin_;
};

/** @param bytes Borrowed strict PSS input copied/parsed during stage construction.
 * @param options Existing layer policy; APPLICATION is default.
 * @param origin Registration site.
 * @return Owning stage. Raw pointers require a valid terminated C string; use a sized view for arbitrary bytes.
 */
inline StyleStage style(std::string_view bytes, const StyleOptions &options = {},
                        std::source_location origin = std::source_location::current()) {
  return StyleStage::bytes(bytes, options, origin);
}

/** @tparam N Full char array extent, including a trailing terminator when present.
 * @param bytes Literal/array bytes; only one final NUL terminator is excluded, embedded NUL is retained.
 * @param options Existing layer policy.
 * @param origin Registration site.
 * @return Owning strict input stage; embedded NUL cannot be silently truncated by strlen.
 */
template <std::size_t N>
StyleStage style(const char (&bytes)[N], const StyleOptions &options = StyleOptions{},
                 std::source_location origin = std::source_location::current()) {
  return style(std::string_view(bytes, N && bytes[N - 1] == '\0' ? N - 1 : N), options, origin);
}

/** @param document Borrowed mutable document, fully validated before an immutable owning copy.
 * @param options Existing layer policy/source identity.
 * @param origin Registration site.
 * @return Owning stage or deferred exact PSS admission failure.
 */
inline StyleStage style(const PssDocument &document, const StyleOptions &options = {},
                        std::source_location origin = std::source_location::current()) {
  return StyleStage::document(document, options, origin);
}

/** @param source Explicit owning path.
 * @param options Existing layer policy/source identity.
 * @param origin Registration site.
 * @return Deferred file stage with strict parser semantics.
 */
inline StyleStage style(const FileSource &source, const StyleOptions &options = {},
                        std::source_location origin = std::source_location::current()) {
  return StyleStage::source(source, options, origin);
}

/** @tparam R Rule capacity of an already checked compact sheet.
 * @tparam C Class capacity of the checked sheet.
 * @tparam D Declaration occurrence capacity of the checked sheet.
 * @tparam B Owning text capacity of the checked sheet.
 * @param description Immutable checked sheet, materialized through existing provenance conversion.
 * @param options Existing layer policy; sourceName overrides only when nonempty.
 * @param origin Registration site.
 * @return Owning immutable stage, or deferred original materialization failure.
 */
template <std::size_t R, std::size_t C, std::size_t D, std::size_t B>
StyleStage style(const style::ct::CompactSheet<R, C, D, B> &description, const StyleOptions &options = {},
                 std::source_location origin = std::source_location::current()) {
  auto document = style::ct::materializeDocument(description);

  return document ? StyleStage::document(*document, options, origin)
                  : StyleStage::failure(document.error(), options, origin);
}

/** @param owner Stationary facade.
 * @param stage Shared owning stage.
 * @param origin Registration site.
 * @return Same facade reference, equivalent to method and pipe application.
 */
inline Scene &style(Scene &owner, const StyleStage &stage,
                    std::source_location origin = std::source_location::current()) {
  return owner.style(stage, origin);
}

// Source overload documentation is on the Scene declaration in ui_scene.hpp.
template <class Source>
  requires std::constructible_from<StyleStage, Source &&, const StyleOptions &, std::source_location>
Scene &Scene::style(Source &&source, const StyleOptions &options, std::source_location origin) & {
  const StyleStage stage(std::forward<Source>(source), options, origin);

  return style(stage, origin);
}

// Source overload documentation is on the Scene declaration in ui_scene.hpp.
template <class Source>
  requires std::constructible_from<StyleStage, Source &&, const StyleOptions &, std::source_location>
Scene &&Scene::style(Source &&source, const StyleOptions &options, std::source_location origin) && {
  static_cast<Scene &>(*this).style(std::forward<Source>(source), options, origin);

  return std::move(*this);
}

/** @tparam Source Input accepted by an owning StyleStage constructor.
 * @param owner Stationary receiving facade.
 * @param source Runtime bytes, checked descriptor, document or explicit file source.
 * @param options Existing layer/source policy.
 * @param origin Registration site.
 * @return The same facade, equivalent to source method and owning stage pipe.
 */
template <class Source>
  requires std::constructible_from<StyleStage, Source &&, const StyleOptions &, std::source_location>
Scene &style(Scene &owner, Source &&source, const StyleOptions &options = {},
             std::source_location origin = std::source_location::current()) {
  return owner.style(std::forward<Source>(source), options, origin);
}

namespace detail {
template <class S> struct LiveDescriptor;

/** @brief Scene-owned actual existing controller with weak publication only.
 * @tparam S Existing validated static stylesheet type.
 */
template <class S> struct LiveController final : LiveBinding, std::enable_shared_from_this<LiveController<S>> {
  std::shared_ptr<LiveDescriptor<S>> descriptor; ///< Owns the descriptor, never an additional UI owner.
  style::ct::LiveTheme<S> controller;            ///< Existing theme controller and authority lease.
  std::shared_ptr<RefTarget> events = std::make_shared<RefTarget>(); ///< Preallocated weak collector observation.

  /** @param value Shared owning descriptor; no scene or theme is attached during construction. */
  explicit LiveController(std::shared_ptr<LiveDescriptor<S>> value) : descriptor(std::move(value)) {
  }

  /** @copydoc LiveBinding::attach */
  std::expected<bool, ManagedError> attach(ManagedScene &scene, const std::shared_ptr<SceneAnchor> &anchor) override {
    if (descriptor->owner != std::this_thread::get_id()) {
      return std::unexpected(ManagedError{ManagedPhase::STYLE, {}, TreeError::WRONG_THREAD, {}, {}});
    }

    if (descriptor->bound) {
      const auto previousAnchor = descriptor->anchor.lock();
      const auto previous = descriptor->binding.lock();

      if (!previousAnchor || !previous) {
        return std::unexpected(ManagedError{ManagedPhase::STYLE, {}, TreeError::CLOSED, {}, {}});
      }

      if (previousAnchor != anchor) {
        return std::unexpected(ManagedError{ManagedPhase::STYLE, {}, TreeError::INVALID_ARGUMENT, {}, {}});
      }

      auto checked = previous->controller.apply();

      return checked ? std::expected<bool, ManagedError>{false} : std::unexpected(checked.error());
    }

    auto attached = style::ct::LiveTheme<S>::attach(scene, descriptor->description);

    if (!attached) {
      return std::unexpected(attached.error());
    }

    controller = *attached;

    events->scene = &scene;
    events->lifetime = style::ct::detail::Access::lifetime(scene);
    events->node = scene.root();

    descriptor->anchor = anchor;
    descriptor->binding = this->weak_from_this();
    descriptor->bound = true;

    // Publication shares only preallocated weak control blocks after the engine commit.
    return true;
  }
};

/** @brief Shared owning descriptor identity, retaining only weak UI binding observations.
 * @tparam S Existing validated static stylesheet type, copied before receiving facade entry.
 */
template <class S> struct LiveDescriptor : LiveFactory, std::enable_shared_from_this<LiveDescriptor<S>> {
  const S description; ///< Immutable owning static sheet, independent of caller storage.
  const std::thread::id owner = std::this_thread::get_id(); ///< Immutable descriptor affinity.
  std::weak_ptr<LiveController<S>> binding;                 ///< Published only after successful attachment.
  std::weak_ptr<SceneAnchor> anchor;                        ///< Original single receiving facade identity.
  bool bound{}; ///< Permanent first-success marker; cleanup never permits rebinding.

  /** @param receiving Receiving facade identity, checked before controller publication.
   * @return Existing binding for reuse, a new owning binding, or an exact admission error.
   */
  std::expected<std::shared_ptr<LiveBinding>, ManagedError>
  create(const std::shared_ptr<SceneAnchor> &receiving) override {
    if (owner != std::this_thread::get_id()) {
      return std::unexpected(ManagedError{ManagedPhase::STYLE, {}, TreeError::WRONG_THREAD, {}, {}});
    }

    if (bound) {
      const auto previous = anchor.lock();
      const auto existing = binding.lock();

      if (!previous || !existing) {
        return std::unexpected(ManagedError{ManagedPhase::STYLE, {}, TreeError::CLOSED, {}, {}});
      }

      if (previous != receiving) {
        return std::unexpected(ManagedError{ManagedPhase::STYLE, {}, TreeError::INVALID_ARGUMENT, {}, {}});
      }

      return std::static_pointer_cast<LiveBinding>(existing);
    }

    return std::static_pointer_cast<LiveBinding>(std::make_shared<LiveController<S>>(this->shared_from_this()));
  }

  /** @param value Existing validated owning static stylesheet copied before any UI binding. */
  explicit LiveDescriptor(const S &value) : description(value) {
  }
};
} // namespace detail

/** @brief Weak typed live parameter observation, never owning a controller or UI owner.
 * @tparam S Existing validated static stylesheet type.
 * Before first successful bind and after facade cleanup operations return CLOSED.
 * Foreign-thread calls reject before mutable descriptor, weak target, or raw ManagedScene access.
 */
template <class S> class LiveThemeRef {
public:
  /** @brief Construct an empty observer; operations return CLOSED. */
  LiveThemeRef() = default;

  /** @tparam Name Known parameter name in S.
   * @param origin Read call site retained in recoverable diagnostics.
   * @return Current scalar or exact typed error; recoverable failures go to a live UI collector.
   */
  template <style::ct::detail::Literal Name>
  auto get(std::source_location origin = std::source_location::current()) const {
    using Result = decltype(std::declval<style::ct::LiveTheme<S>>().template get<Name>());
    auto binding = lock();

    if (!binding) {
      return Result(std::unexpected(binding.error()));
    }

    auto result = (*binding)->controller.template get<Name>();

    if (!result) {
      detail::reportEventError((*binding)->events, result.error(), origin);
    }

    return result;
  }

  /** @tparam Name Known parameter name in S.
   * @tparam T Exact existing parameter scalar type.
   * @param value Candidate scalar, admitted atomically through existing LiveTheme.
   * @param origin Update site retained in recoverable diagnostics.
   * @return True for change, false for equality, or exact failure without facade sticky failure.
   */
  template <style::ct::detail::Literal Name, class T>
  std::expected<bool, ManagedError> set(T value, std::source_location origin = std::source_location::current()) const {
    return applyAt(origin, style::ct::change<Name>(value));
  }

  /** @tparam C Existing named update types; names unique and values exact.
   * @param changes Candidates committed together through the existing controller.
   * @return True for change, false for equality, or exact recoverably reported failure.
   */
  template <class... C> std::expected<bool, ManagedError> apply(C... changes) const {
    return applyAt(std::source_location::current(), changes...);
  }

private:
  template <class> friend class LiveThemeStage;

  LiveThemeRef(std::weak_ptr<detail::LiveDescriptor<S>> descriptor, std::thread::id owner)
      : descriptor_(std::move(descriptor)), owner_(owner) {
  }

  std::expected<std::shared_ptr<detail::LiveController<S>>, ManagedError> lock() const {
    if (owner_ != std::thread::id{} && owner_ != std::this_thread::get_id()) {
      return std::unexpected(ManagedError{ManagedPhase::STYLE, {}, TreeError::WRONG_THREAD, {}, {}});
    }

    const auto descriptor = descriptor_.lock();
    const auto binding = descriptor ? descriptor->binding.lock() : nullptr;

    if (!binding || !descriptor->bound || descriptor->anchor.expired()) {
      return std::unexpected(ManagedError{ManagedPhase::STYLE, {}, TreeError::CLOSED, {}, {}});
    }

    auto admitted = detail::eventScene(binding->events);

    return admitted ? std::expected<std::shared_ptr<detail::LiveController<S>>, ManagedError>{binding}
                    : std::unexpected(admitted.error());
  }

  template <class... C> std::expected<bool, ManagedError> applyAt(std::source_location origin, C... changes) const {
    auto binding = lock();

    if (!binding) {
      return std::unexpected(binding.error());
    }

    auto result = (*binding)->controller.apply(changes...);

    if (!result) {
      detail::reportEventError((*binding)->events, result.error(), origin);
    }

    return result;
  }

  std::weak_ptr<detail::LiveDescriptor<S>> descriptor_;
  std::thread::id owner_{};
};

/** @brief Copyable owning static description with one single-scene live binding identity.
 * @tparam S Existing validated static sheet type; no controller or UI ownership is retained by the stage.
 * Failed first attachment stays unbound; same-scene reuse checks the original lease, another scene rejects.
 * Cleanup permanently closes the identity; it never rebinds to another scene.
 */
template <class S> class LiveThemeStage {
public:
  /** @param other Shared owning descriptor identity; rvalue transfer also preserves the source identity. */
  LiveThemeStage(const LiveThemeStage &other) noexcept = default;

  /** @param other Descriptor identity to share without retaining a UI owner.
   * @return This stage after a nonallocating identity copy.
   */
  LiveThemeStage &operator=(const LiveThemeStage &other) noexcept = default;

  /** @param description Existing owning static sheet copied before facade entry.
   * @param origin Stage registration site.
   */
  explicit LiveThemeStage(const S &description, std::source_location origin)
      : descriptor_(std::make_shared<detail::LiveDescriptor<S>>(description)), origin_(origin) {
  }

  /** @return Weak typed parameter observation, CLOSED before attachment or after cleanup. */
  LiveThemeRef<S> ref() const {
    return {descriptor_, descriptor_->owner};
  }

  /** @tparam Name Existing known parameter name.
   * @param origin Read call site retained in recoverable diagnostics.
   * @return Same checked value/error as the weak ref.
   */
  template <style::ct::detail::Literal Name>
  auto get(std::source_location origin = std::source_location::current()) const {
    return ref().template get<Name>(origin);
  }

  /** @tparam Name Existing known parameter name.
   * @tparam T Exact scalar type.
   * @param value Candidate scalar.
   * @param origin Update site.
   * @return Same checked atomic result as the weak ref.
   */
  template <style::ct::detail::Literal Name, class T>
  std::expected<bool, ManagedError> set(T value, std::source_location origin = std::source_location::current()) const {
    return ref().template set<Name>(value, origin);
  }

  /** @tparam C Existing exact named update types.
   * @param changes One atomic candidate batch.
   * @return Same checked result as the weak ref.
   */
  template <class... C> std::expected<bool, ManagedError> apply(C... changes) const {
    return ref().apply(changes...);
  }

  /** @param owner Stationary receiving facade.
   * @param stage Shared owning descriptor identity.
   * @return Same facade after one checked binding registration.
   */
  friend Scene &operator|(Scene &owner, const LiveThemeStage &stage) {
    // Allocate the controller/publication candidate within the facade's checked configuration,
    // before attachment can mutate the existing theme. Captures share only descriptor ownership.
    stage.install(owner);

    return owner;
  }

  /** @param input Consumed idle facade, moved before stage dispatch.
   * @param stage Shared owning descriptor.
   * @return Owning facade for temporary chains.
   */
  friend Scene operator|(Scene &&input, const LiveThemeStage &stage) {
    Scene owner(std::move(input));

    stage.install(owner);

    return owner;
  }

private:
  void install(Scene &owner) const {
    detail::SceneAccess::live(owner, descriptor_, origin_);
  }

  std::shared_ptr<detail::LiveDescriptor<S>> descriptor_;
  std::source_location origin_;
};

/** @tparam S Existing validated owning static sheet.
 * @param description Existing static descriptor, copied into one owning stage identity.
 * @param origin Stage registration site.
 * @return Owning copyable stage with weak .ref(); Scene retains the actual controller.
 */
template <class S>
  requires style::ct::detail::IsSheet<S>::value
LiveThemeStage<S> liveTheme(const S &description, std::source_location origin = std::source_location::current()) {
  return LiveThemeStage<S>(description, origin);
}
} // namespace portals::ui
