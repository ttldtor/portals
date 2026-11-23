// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_model_edit.hpp>

#include <concepts>
#include <string>
#include <utility>

namespace {
struct Record {
  std::string name;
  int count;

  bool operator==(const Record &) const = default;
};

using Session = decltype(portals::ui::edit(std::declval<portals::ui::Navigation<Record>>()));
static_assert(std::copy_constructible<Session>);
static_assert(requires(Session session) {
  session.field(&Record::name);
  session.field(&Record::count);
  session.commit();
  session.cancel();
});
} // namespace
