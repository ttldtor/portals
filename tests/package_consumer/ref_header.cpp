// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_ref.hpp>

namespace {
template <class T>
concept TextReference = requires(const portals::ui::Ref<T> &reference) { reference.text(); };

template <class T>
concept ClickReference = requires(const portals::ui::Ref<T> &reference) { reference.onClick([] {}); };

static_assert(TextReference<portals::Label> && TextReference<portals::LineEdit>);
static_assert(!TextReference<portals::Widget> && !TextReference<portals::Password>);
static_assert(ClickReference<portals::Button> && !ClickReference<portals::Label>);
static_assert(sizeof(portals::ui::Ref<portals::Label>) == sizeof(portals::ui::NodeRef<portals::Label>));
static_assert(sizeof(portals::ui::NodeRef<portals::Label>) == sizeof(std::shared_ptr<void>));
} // namespace

// Separate translation units check Scene-only use and both composite include orders.
bool sceneHeaderContracts();
bool refHeaderOrderBeforeContracts();
bool refHeaderOrderAfterContracts();

int main() {
  portals::ui::Ref<portals::Label> label;
  portals::ui::NodeRef<portals::Label> node;
  const auto geometry = label.geometry();
  const auto text = label.text().get();

  if (node.node() != portals::Node<portals::Label>{} || geometry || text) {
    return 1;
  }

  const auto *geometryCause = std::get_if<portals::TreeError>(&geometry.error().cause);
  const auto *textCause = std::get_if<portals::TreeError>(&text.error().cause);

  if (!geometryCause || *geometryCause != portals::TreeError::CLOSED || !textCause ||
      *textCause != portals::TreeError::CLOSED) {
    return 2;
  }

  return sceneHeaderContracts() && refHeaderOrderBeforeContracts() && refHeaderOrderAfterContracts() ? 0 : 3;
}
