// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "key_binding_state.hpp"
#include <portals/key_hints.hpp>
#include <portals/ui.hpp>

namespace portals::detail {
struct KeyHelpText {
  std::optional<GraphemeText> caption;
  std::vector<std::optional<GraphemeText>> lines = std::vector<std::optional<GraphemeText>>(0);
  std::size_t overflow{};
};

struct KeyHelpPresentation {
  explicit KeyHelpPresentation(GraphemeText marker) : unavailable(std::move(marker)) {
  }

  std::shared_ptr<const KeyBindingsSnapshot> projection;
  std::shared_ptr<const KeyHelpText> text;
  GraphemeText unavailable;
  bool current{};
};

struct KeyHelpRecord {
  enum class Phase {
    CLOSED,
    OPENING,
    OPEN,
    CLOSING
  };
  Node<Widget> root;
  Node<Widget> body;
  Node<Widget> help;
  std::shared_ptr<KeyHelpPresentation> presentation;
  std::optional<KeyHintsOptions> hints;
  std::optional<HelpViewOptions> full;
  std::array<std::uint64_t, 6> stamp{};
  std::uint64_t activation{};
  std::weak_ptr<ActionCancellation> scope;
  std::size_t textBytes{};
  std::size_t projectionBytes{};
  Phase phase{Phase::CLOSED};
};

class HelpBody final : public SizedWidget {
public:
  explicit HelpBody(std::shared_ptr<KeyHelpPresentation> value, ControlStyle style = {})
      : presentation_(std::move(value)), style_(style) {
  }

  std::expected<CellSize, TreeError> measure(CellSize available) override;
  std::expected<void, TreeError> arrange(CellRect local) override;
  std::expected<void, TreeError> paint(PaintContext &context) override;

  ComputedStyle styleDefaults() const override {
    ComputedStyle value;
    value.normal = style_.normal;
    value.focused = style_.focused;
    value.disabled = style_.disabled;
    return value;
  }

private:
  std::shared_ptr<KeyHelpPresentation> presentation_;
  ControlStyle style_;
  CellRect bounds_;
};
} // namespace portals::detail

namespace portals::ui::detail {
struct KeyHelpAccess {
  static Description<HelpView> full(Ref<HelpView> reference, HelpViewOptions options, ControlStyle style);
  static Description<KeyHints> hints(Ref<HelpView> help, KeyHintsOptions options, ControlStyle style);
  // Resource-local erased observers still use the native checked-target finalizer.
  static Description<KeyHints> hints(Ref<Widget> help, KeyHintsOptions options, ControlStyle style);
  static std::expected<GraphemeText, ManagedError> validate(ManagedScene &scene, const HelpViewOptions &options,
                                                            Node<Widget> parent = {},
                                                            std::string_view *property = nullptr);
  static std::expected<GraphemeText, ManagedError> validate(ManagedScene &scene, const KeyHintsOptions &options,
                                                            Node<Widget> parent = {},
                                                            std::string_view *property = nullptr);
  static Description<KeyHints> hintsTarget(std::shared_ptr<const RefTarget> target, KeyHintsOptions options,
                                           ControlStyle style);
  static std::expected<void, ManagedError> command(ManagedScene &scene, Node<Widget> node, bool opening);
};
} // namespace portals::ui::detail
