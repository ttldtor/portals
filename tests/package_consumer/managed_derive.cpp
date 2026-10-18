// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace {
template <class T, class E> T checked(std::expected<T, E> result) {
  if (!result) {
    std::exit(EXIT_FAILURE);
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}
template <class T>
concept Writable = requires(T value) { value.set(std::string{}); };
static_assert(!Writable<portals::ManagedSource<std::string>>);
static_assert(!std::convertible_to<portals::ManagedSource<std::string>, portals::ManagedState<std::string>>);

struct ValueObserver {
  void operator()(const int &) const {
  }
};

struct CheckedValueObserver {
  std::expected<void, portals::TreeError> operator()(const int &) const {
    return {};
  }
};

struct WrongArgumentObserver {
  void operator()(std::string_view) const {
  }
};

struct WrongResultObserver {
  int operator()(const int &) const {
    return 0;
  }
};

struct MoveOnlyObserver {
  std::unique_ptr<int> value;

  void operator()(const int &) const {
  }
};
template <class Callback>
concept RootDerivedObserver = requires(portals::ManagedScene &owner, portals::ManagedSource<int> value,
                                       Callback callback) { owner.observe(value, callback); };
template <class Callback>
concept ReceiverDerivedObserver =
  requires(portals::ManagedScene &owner, portals::Node<portals::Label> receiver, portals::ManagedSource<int> value,
           Callback callback) { owner.observe(receiver, value, callback); };
template <class Callback>
concept DerivedObserver = RootDerivedObserver<Callback> && ReceiverDerivedObserver<Callback>;
static_assert(DerivedObserver<ValueObserver>);
static_assert(DerivedObserver<CheckedValueObserver>);
static_assert(!RootDerivedObserver<WrongArgumentObserver> && !ReceiverDerivedObserver<WrongArgumentObserver>);
static_assert(!RootDerivedObserver<WrongResultObserver> && !ReceiverDerivedObserver<WrongResultObserver>);
static_assert(!RootDerivedObserver<MoveOnlyObserver> && !ReceiverDerivedObserver<MoveOnlyObserver>);
} // namespace

/** @brief Verify derived sources and readonly caption/observer adapters through an installed package. */
int main() {
  using namespace portals;
  auto app = checked(Application::create());
  auto owner = checked(ManagedScene::create(*app));
  auto model = checked(owner->state(false));
  int computes{}, presentations{};
  auto rejected = owner->derive(model, [](const bool &) -> std::expected<int, TreeError> {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  });
  if (rejected) {
    return 1;
  }
  auto caption = checked(owner->derive(model, [&](const bool &enabled) {
    ++computes;
    return std::string(enabled ? "Enabled" : "Disabled");
  }));
  ui::Ref<Label> label;
  ui::Ref<Button> button;
  checked(ui::mount(*owner, ui::vbox(ui::label(caption).reference(label).observe(caption,
                                                                                 [&](const std::string &) {
                                                                                   ++presentations;
                                                                                 }),
                                     ui::button(caption, [] {
                                     }).reference(button))));
  checked(owner->prepare(*app, {40, 8}));
  if (computes != 1 || presentations != 1 || checked(caption.snapshot())->value != "Disabled") {
    return 2;
  }
  checked(model.set(true));
  checked(owner->prepare(*app, {40, 8}));
  if (computes != 2 || presentations != 2 || checked(label.captionText().snapshot()).bytes() != "Enabled" ||
      checked(button.captionText().snapshot()).bytes() != "Enabled") {
    return 3;
  }
  checked(model.set(true));
  checked(owner->prepare(*app, {40, 8}));
  if (computes != 2 || checked(caption.snapshot())->revision != 1) {
    return 4;
  }
  auto pinned = checked(caption.snapshot());
  checked(owner->close());
  if (checked(caption.snapshot())->value != "Enabled") {
    return 5;
  }
  owner.reset();
  return !caption.snapshot() && caption.snapshot().error() == TreeError::CLOSED && pinned->value == "Enabled" ? 0 : 6;
}
