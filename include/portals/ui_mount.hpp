// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_layout.hpp>

namespace portals::ui {

/** @brief Consume a recipe into the existing managed subtree, with failure cleanup.
 * @tparam T Concrete root Widget type constructed by the recipe.
 * @param scene Existing UI-affine owner, outliving all mounted widgets.
 * @param description Owning rvalue recipe, consumed even when mounting fails.
 * @param controls Owning immutable defaults snapshot validated before construction.
 * @return Observing root or original typed failure plus independent cleanup error. Earlier
 * scene nodes and states survive. Reusing an empty/moved/consumed recipe returns INVALID_ARGUMENT.
 * References publish only after all nodes remain live. External callback effects are not undone.
 */
template <class T>
std::expected<Node<T>, ManagedError> mount(ManagedScene &scene, Description<T> &&description,
                                           ControlContext controls) try {
  auto admitted = detail::Access::check(scene, scene.root());
  if (!admitted) {
    auto consumed = std::move(description);
    return std::unexpected(admitted.error());
  }

  if (detail::Access::needsGridContext(description)) {
    auto installed = detail::ControlContextAccess::installFallbackSchema(controls);

    if (!installed) {
      auto consumed = std::move(description);
      return std::unexpected(detail::controlContextError(installed.error(), scene.root()));
    }
  }

  if (detail::ControlContextAccess::hasUserConfiguration(controls)) {
    for (const auto name : {"Border", "Gauge", "Spinner"}) {
      auto installed = detail::ControlContextAccess::installPrimitiveSchema(controls, name);

      if (!installed) {
        auto consumed = std::move(description);

        return std::unexpected(detail::controlContextError(installed.error(), scene.root()));
      }
    }

    for (const auto name : {"ScrollArea", "ScrollBar", "LogView"}) {
      auto installed = detail::ControlContextAccess::installScrollSchema(controls, name);

      if (!installed) {
        auto consumed = std::move(description);

        return std::unexpected(detail::controlContextError(installed.error(), scene.root()));
      }
    }
    for (const auto name : {"HelpView", "KeyHints"}) {
      auto installed = detail::ControlContextAccess::installKeyHelpSchema(controls, name);
      if (!installed) {
        auto consumed = std::move(description);
        return std::unexpected(detail::controlContextError(installed.error(), scene.root()));
      }
    }
    for (const auto name : {"VirtualList", "VirtualTable", "Chart"}) {
      auto installed = detail::ControlContextAccess::installVirtualViewSchema(controls, name);

      if (!installed) {
        auto consumed = std::move(description);
        return std::unexpected(detail::controlContextError(installed.error(), scene.root()));
      }
    }

    for (const auto name : {"OverlayHost", "Dialog"}) {
      auto installed = detail::ControlContextAccess::installDialogSchema(controls, name);

      if (!installed) {
        auto consumed = std::move(description);

        return std::unexpected(detail::controlContextError(installed.error(), scene.root()));
      }
    }

    auto notificationInstalled = detail::ControlContextAccess::installNotificationSchema(controls);

    if (!notificationInstalled) {
      auto consumed = std::move(description);

      return std::unexpected(detail::controlContextError(notificationInstalled.error(), scene.root()));
    }

    auto selectorInstalled = detail::ControlContextAccess::installThemeCatalogSchema(controls);

    if (!selectorInstalled) {
      auto consumed = std::move(description);
      return std::unexpected(detail::controlContextError(selectorInstalled.error(), scene.root()));
    }

    auto messageInstalled = detail::ControlContextAccess::installMessageSchema(controls);

    if (!messageInstalled) {
      auto consumed = std::move(description);

      return std::unexpected(detail::controlContextError(messageInstalled.error(), scene.root()));
    }

    auto dataStateInstalled = detail::ControlContextAccess::installDataStateSchema(controls);

    if (!dataStateInstalled) {
      auto consumed = std::move(description);

      return std::unexpected(detail::controlContextError(dataStateInstalled.error(), scene.root()));
    }

    auto installed = detail::ControlContextAccess::installOverlaySchema(controls);
    if (!installed) {
      auto consumed = std::move(description);
      return std::unexpected(detail::controlContextError(installed.error(), scene.root()));
    }
  }

  auto prepared = detail::Access::defersControlValidation(description)
                    ? std::expected<ControlContext, ControlContextError>(controls)
                    : controls.prepared();
  if (!prepared) {
    auto consumed = std::move(description);
    return std::unexpected(detail::controlContextError(prepared.error(), scene.root()));
  }

  struct Mounting {
    ManagedScene &scene;

    ~Mounting() {
      detail::Access::endMount(scene);
    }
  } guard{scene};

  detail::Access::beginMount(scene);
  detail::MountContext context;
  context.controls = std::move(*prepared);
  std::expected<Node<T>, ManagedError> result =
    std::unexpected(detail::error(scene.root(), TreeError::INVALID_ARGUMENT));

  try {
    result = detail::Access::mount(std::move(description), scene, scene.root(), context);

    if (result) {
      for (auto &finalize : context.finalize) {
        auto completed = finalize();
        if (!completed) {
          result = std::unexpected(completed.error());
          break;
        }
      }
    }
    if (result) {
      for (auto node : context.nodes) {
        auto live = detail::Access::check(scene, node);

        if (!live) {
          result = std::unexpected(live.error());

          break;
        }
      }
    }
  } catch (const std::bad_alloc &) {
    result = std::unexpected(detail::error(context.root, TreeError::NO_MEMORY));
  } catch (...) {
    result = std::unexpected(detail::error(context.root, TreeError::CALLBACK_FAILED));
  }

  if (!result && context.root != Node<Widget>{}) {
    auto removed = scene.remove(context.root);

    if (!removed) {
      if (const auto *cause = std::get_if<TreeError>(&removed.error().cause)) {
        if (*cause != TreeError::STALE_NODE && *cause != TreeError::CLOSED) {
          result.error().cleanup = *cause;
        }
      }
    }
  }

  if (result) {
    context.committed = true;
    for (auto &publish : context.publish) {
      publish();
    }
  }

  return result;
} catch (const std::bad_alloc &) {
  // Transaction storage is constructed before any node exists; preserve the checked allocation boundary too.
  auto consumed = std::move(description);

  return std::unexpected(detail::error(scene.root(), TreeError::NO_MEMORY));
}

/** @param scene Existing UI owner. @param description Consuming recipe using builtin settings only.
 * @return Existing checked mount result; explicit contexts use the three-argument overload.
 */
template <class T> std::expected<Node<T>, ManagedError> mount(ManagedScene &scene, Description<T> &&description) {
  return mount(scene, std::move(description), ControlContext{});
}

} // namespace portals::ui
