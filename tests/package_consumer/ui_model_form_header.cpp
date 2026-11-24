// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_model_form.hpp>

#include <array>

namespace {
struct Record {
  std::string name;
  int orders{};
  double rate{};
  bool operator==(const Record &) const = default;
};

[[maybe_unused]] void formHeaderConsumer() {
  auto model = portals::ui::model(std::array{Record{"one", 1, 1.5}});
  auto edit = portals::ui::edit(portals::ui::navigation(model));
  auto form = portals::ui::form(edit, portals::ui::field("Name", &Record::name).required(),
                                portals::ui::field("Orders", &Record::orders),
                                portals::ui::field("Rate", &Record::rate).readOnly());
  (void)form;
}

static_assert(requires { &formHeaderConsumer; });
} // namespace
