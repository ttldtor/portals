// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_model.hpp>

#include <algorithm>
#include <initializer_list>
#include <ranges>

namespace portals::ui {
/** @brief Project an accepted navigation resource to owning domain metadata.
 * @tparam Row Collection row shared by the source declarations; no row values are copied or read.
 * @tparam Value Copyable owning metadata; string by default, also supporting numbers, enums and records.
 * @param entries Source/metadata pairs copied now, without registering cold sources or reading providers.
 * @param limits Existing positive collection count cap, applied to catalog entries rather than model rows.
 * @return Owned pure projection for label/button(source, projection). Lookup is linear in the bounded
 * source catalog, independent of row count. Unknown resources, invalid/duplicate sources and oversized
 * catalogs return typed errors when invoked; there is no fallback to an unrelated source's metadata.
 * Registration is inspected only on its creating UI thread, including the enclosing mount's unpublished
 * registration. Cold entries are skipped, retired registrations and mixed live scenes are rejected.
 * Captured metadata must own its reachable storage; known borrowed views and pointers are rejected.
 */
template <class Row, class Value = std::string>
  requires std::copy_constructible<Value> && (!std::is_pointer_v<Value>) &&
           (!portals::detail::IS_BORROWED_DERIVED_VALUE<Value>) && (!std::ranges::view<Value>)
auto bySource(std::initializer_list<std::pair<Model<Row>, Value>> entries, CollectionLimits limits = {}) {
  using Entries = std::vector<std::pair<Model<Row>, Value>>;
  std::shared_ptr<const Entries> retained;
  TreeError failure = TreeError::INVALID_ARGUMENT;

  try {
    if (limits.maxRows && entries.size()) {
      if (entries.size() > limits.maxRows) {
        failure = TreeError::NODE_LIMIT;
      } else {
        auto owned = std::make_shared<Entries>(entries);
        std::vector<const void *> identities(0);

        identities.reserve(owned->size());
        for (const auto &entry : *owned) {
          const auto identity = detail::ModelAccess::identity(entry.first);

          if (identity) {
            identities.push_back(identity.get());
          }
        }

        std::ranges::sort(identities, std::less<const void *>{});
        const bool duplicate = std::ranges::adjacent_find(identities) != identities.end();
        if (!duplicate) {
          retained = std::move(owned);
        }
      }
    }
  } catch (const std::bad_alloc &) {
    failure = TreeError::NO_MEMORY;
  } catch (...) {
    failure = TreeError::CALLBACK_FAILED;
  }

  return
    [retained = std::move(retained), failure](const NavigationPosition &position) -> std::expected<Value, TreeError> {
      if (!retained) {
        return std::unexpected(failure);
      }
      if (!position.resource.identity.instance || !position.resource.identity.epoch || !position.resource.lineage) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      std::optional<NodeId> scene;
      const Value *matched{};

      for (const auto &[model, value] : *retained) {
        auto registered = detail::ModelAccess::registeredSource(model);
        if (!registered) {
          return std::unexpected(registered.error());
        }
        if (!*registered) {
          continue;
        }
        if (scene && *scene != (**registered).scene) {
          return std::unexpected(TreeError::WRONG_SESSION);
        }

        scene = (**registered).scene;
        if ((**registered).resource == position.resource) {
          if (matched) {
            return std::unexpected(TreeError::INVALID_ARGUMENT);
          }

          matched = &value;
        }
      }
      if (!matched) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      try {
        return *matched;
      } catch (const std::bad_alloc &) {
        return std::unexpected(TreeError::NO_MEMORY);
      } catch (...) {
        return std::unexpected(TreeError::CALLBACK_FAILED);
      }
    };
}
} // namespace portals::ui
