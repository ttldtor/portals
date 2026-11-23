// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_controls.hpp>

#include <concepts>
#include <expected>
#include <optional>
#include <span>

template <class T>
concept CaptionSource = requires(T source) {
  portals::ui::label(source);
  portals::ui::button(source);
};

static_assert(CaptionSource<portals::ui::ModelValue<int>>);
static_assert(!CaptionSource<std::span<int>>);

static_assert(requires(portals::ui::Ref<portals::Label> label, portals::ui::Ref<portals::Button> button,
                       portals::ui::Ref<portals::Border> frame) {
  portals::ui::label({});
  portals::ui::button({});
  portals::ui::label(label, {});
  portals::ui::button(button, {});
  portals::ui::label("body").framed("ascii", {});
  portals::ui::label("body").framed(frame, "ascii", {});
});

static_assert(requires(const portals::ui::Model<int> &model,
                       const portals::ui::Navigation<int> &navigation, portals::RowId id) {
  { model.update(id, 1) } -> std::same_as<std::expected<void, portals::ManagedError>>;
  { model.row(id, 1) } -> std::same_as<bool>;
  { navigation.currentRow() }
    -> std::same_as<std::expected<std::optional<portals::RowId>, portals::ManagedError>>;
});

void constructValueCaptions() {
  auto value = portals::ui::model(1);
  auto label = portals::ui::label(value);
  auto button = portals::ui::button(portals::text("{}", value));
  auto propertyButton = portals::ui::button(value.value(), [] {
  });
  auto framed = portals::ui::label("body").framed("ascii", portals::text("{}", value));

  (void)label;
  (void)button;
  (void)propertyButton;
  (void)framed;
}
