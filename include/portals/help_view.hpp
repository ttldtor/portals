// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/key_binding.hpp>
#include <portals/primitives.hpp>

namespace portals {
/** @brief Owning ASCII-first formatter settings; glyph support is never inferred. */
struct KeyFormatOptions {
  /** @brief Release the owning formatter strings. */
  ~KeyFormatOptions() = default;
  std::size_t maxBytesPerBinding{8192};    ///< Positive whole formatted row cap.
  std::string strokeSeparator{" then "};   ///< Between sequence strokes.
  std::string alternativeSeparator{" / "}; ///< Between alternatives.
  std::string repeatMarker{" [repeat]"};   ///< Actual native repeat phase.
  std::string releaseMarker{" [release]"}; ///< Actual native release phase.
  std::string unknownMarker{" [?]"};       ///< Terminal support is unknown.
};

/** @brief Ordinary help pane options; open/close bindings belong to the application. */
struct HelpViewOptions {
  /** @brief Release the owning presentation options. */
  ~HelpViewOptions() = default;
  bool initiallyOpen{};                            ///< False by default; opening uses the existing focus-return stack.
  KeyFormatOptions format{};                       ///< Owning bounded row formatter.
  std::string emptyText{"No key bindings"};        ///< Empty-list presentation.
  std::string unavailableText{"Help unavailable"}; ///< Failed fresh projection marker.
};

/** @brief Ordinary nonmodal focus-scope container; its sole content is an existing ScrollArea. */
class HelpView : public Column {
public:
  /** @param policy Existing layout constraints. */
  explicit HelpView(SizePolicy policy = {}) : Column({}, policy) {
  }
};

namespace ui {
template <class T> class Ref;
template <class T> class Description;
/** @param reference Deferred observing publication. @param options Owning settings.
 * @param style Body presentation defaults. @return Initially hidden ordinary help recipe. */
Description<HelpView> helpView(Ref<HelpView> reference, HelpViewOptions options = {}, ControlStyle style = {});

namespace detail {
struct RefTarget;
struct KeyHelpAccess;
/** @param target Original observing identity. @param opening Select open/close.
 * @param origin Call site. @return Pure checked result; never reports a new failure itself. */
std::expected<void, ManagedError> helpCommand(const std::shared_ptr<RefTarget> &target, bool opening,
                                              std::source_location origin);
} // namespace detail
} // namespace ui
} // namespace portals
