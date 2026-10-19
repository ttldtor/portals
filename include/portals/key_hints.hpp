// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/help_view.hpp>

namespace portals {
namespace detail {
struct KeyHelpPresentation;
}

/** @brief Single-row compact presentation options, bounded by the canonical registration limit. */
struct KeyHintsOptions {
  /** @brief Release the owning compact presentation options. */
  ~KeyHintsOptions() = default;
  KeyFormatOptions format{};                    ///< Owning bounded formatter options.
  std::string entrySeparator{"  "};             ///< Between complete named entries.
  std::string overflowMarker{"+"};              ///< Marker followed by the number of omitted discoverable rows.
  std::string emptyText = std::string(0, '\0'); ///< Empty-list presentation, empty by default.
  std::string unavailableText{"?"};             ///< Failed fresh projection marker, clipped at grapheme boundaries.
};

/** @brief One-line named-key projection; only a visible overflow action can receive focus. */
class KeyHints : public Button {
public:
  /** @param text Prepared owning caption. @param style Manual presentation defaults. */
  KeyHints(GraphemeText text, ControlStyle style = {}) : Button(std::move(text), style, {}, ButtonChrome::NONE) {
  }

  /** @param available Offered allocation. @return Prepared one-row size without formatting or allocation. */
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  /** @param local Allocated extent. @return Checked retained bounds. */
  std::expected<void, TreeError> arrange(CellRect local) override;
  /** @param context Existing synchronous sink. @return Prepared caption or preallocated unavailable marker. */
  std::expected<void, TreeError> paint(PaintContext &context) override;

private:
  friend struct ui::detail::KeyHelpAccess;
  std::shared_ptr<detail::KeyHelpPresentation> presentation_;
  CellRect bounds_;
};

namespace ui {
/** @param help Deferred help observation, resolved only after complete mount.
 * @param options Owning compact formatter settings. @param style Manual presentation defaults.
 * @return Single-row projection of the scene's real canonical bindings. */
Description<KeyHints> keyHints(Ref<HelpView> help, KeyHintsOptions options = {}, ControlStyle style = {});
} // namespace ui
} // namespace portals
