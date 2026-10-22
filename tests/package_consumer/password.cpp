// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_password.hpp>

#include <cstdlib>
#include <type_traits>

namespace {
using namespace portals;

template <class T, class E> T checked(std::expected<T, E> result) {
  if (!result) {
    std::exit(EXIT_FAILURE);
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

template <class T>
concept HasText = requires(T value) { value.text(); };
template <class T>
concept HasUndo = requires(T value) { value.undo(); };
static_assert(!HasText<ui::PasswordRef>);
static_assert(!HasUndo<ui::PasswordRef>);
} // namespace

/** @brief Exercise the installed protected value, ordinary ref-first form and safe resource diagnostics without a
 * terminal. */
int main() {
  using namespace portals;
  auto retained = checked(PasswordValue::create("installed secret"));
  bool matched{};

  checked(retained.withPlaintext([&](SecretView bytes) {
    matched = std::string_view(bytes.data(), bytes.size()) == "installed secret";
  }));

  if (!matched || !retained.available()) {
    return 2;
  }

  auto app = checked(Application::create());
  auto owner = checked(ManagedScene::create(*app));
  ui::PasswordRef password;
  ui::Ref<Button> submit;
  bool submitted{};

  checked(ui::mount(*owner, ui::vbox(ui::password(password).placeholder("Password").noEcho(),
                                     ui::button(submit, "Submit", [password, &submitted] {
                                       return password.withPlaintext([&](SecretView bytes) {
                                         submitted = std::string_view(bytes.data(), bytes.size()) == "installed secret";
                                       });
                                     }))));
  checked(password.value(retained));
  checked(app->router().invoke(submit.node(), {Action::ACTIVATE}));

  if (!submitted || checked(password.value()) != retained) {
    return 3;
  }

  checked(owner->close());
  auto resource = ui::scene(R"(Password#psw{text="" placeHolder="Password"})");
  auto resourcePassword = resource.get<Password>("psw");

  checked(resource.prepareChecked());

  if (!checked(resourcePassword.value()).empty()) {
    return 4;
  }

  checked(resource.close());
  auto unsafe = ui::scene(R"(Password{text="secret must never become source identity"})");
  auto rejected = unsafe.prepareChecked();

  if (rejected || !rejected.error().sceneResource || !rejected.error().sceneResource->source ||
      *rejected.error().sceneResource->source != "<string resource>") {
    return 5;
  }

  return 0;
}
