// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/controls.hpp>

#include <array>
#include <ostream>
#include <vector>

namespace checkbox_example {
/** @brief Per-child receipts and the aggregate read back after all attempted commits. */
struct GroupUpdate {
  std::size_t committed{};                                                                          ///< Changed child commits, including commits whose notifications were rejected.
  std::size_t unchanged{};                                                                          ///< Successful equal-value sets with no new commit.
  std::size_t failed{};                                                                             ///< Child sets that failed without committing.
  std::size_t rejectedNotifications{};                                                              ///< Rejected notification count, independent of committed/failed children.
  std::size_t cancelledNotifications{};                                                             ///< Cancelled notification count from successful receipts.
  std::vector<portals::CheckStateResult> children;                                                  ///< Input order, including each real error/rejection receipt.
  std::expected<portals::CheckState, portals::TreeError> aggregate{portals::CheckState::UNCHECKED}; ///< Latest snapshots.
};

/** @brief Apply an explicit select-all/clear-all policy without claiming a transaction.
 * @param models Borrowed UI model owners, consulted synchronously in order; nothing is retained.
 * @param requested CHECKED or UNCHECKED; MIXED is computed from the child snapshots.
 * @return Owning receipts and aggregate, or INVALID_ARGUMENT for the requested state / NO_MEMORY
 * before any writes if receipt storage cannot allocate. Child errors do not stop subsequent attempts.
 * Notification rejection is a successful commit in its child's PropertyUpdate, never a rollback.
 * Existing model/tree affinity, lifetime, observer and invalidation rules still apply.
 */
inline std::expected<GroupUpdate, portals::TreeError>
updateSelection(std::span<const std::shared_ptr<portals::Property<portals::CheckState>>> models,
                portals::CheckState requested) {
  using namespace portals;

  if (requested != CheckState::CHECKED && requested != CheckState::UNCHECKED) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  GroupUpdate result;

  try {
    result.children.reserve(models.size());
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }

  for (const auto &model : models) {
    result.children.push_back(model ? model->set(requested) : CheckStateResult(std::unexpected(TreeError::INVALID_ARGUMENT)));

    const auto &receipt = result.children.back();

    if (!receipt) {
      ++result.failed;

      continue;
    }

    if (receipt->changed) {
      ++result.committed;
    } else {
      ++result.unchanged;
    }

    result.rejectedNotifications += receipt->rejected;
    result.cancelledNotifications += receipt->cancelled;
  }

  bool first = true;

  for (const auto &model : models) {
    if (!result.aggregate) {
      break;
    }

    if (!model) {
      result.aggregate = std::unexpected(TreeError::INVALID_ARGUMENT);

      break;
    }

    auto snapshot = model->snapshot();

    if (!snapshot) {
      result.aggregate = std::unexpected(snapshot.error());

      break;
    }

    const std::array pair{first ? (*snapshot)->value : *result.aggregate, (*snapshot)->value};

    result.aggregate = aggregateCheckState(pair);
    first = false;
  }

  return result;
}

/** @brief Display separate counters without discarding the caller's owning receipts.
 * @param update Completed group operation retained by the caller, including every failed child.
 * @param output Borrowed synchronous stream; ordinary stream state/exceptions report output failure.
 * @note Reporting cannot roll back commits; keep update to inspect child and aggregate errors.
 */
inline void reportGroupUpdate(const GroupUpdate &update, std::ostream &output) {
  output << "committed=" << update.committed << " unchanged=" << update.unchanged
         << " failed=" << update.failed << " rejected-notifications=" << update.rejectedNotifications
         << " cancelled-notifications=" << update.cancelledNotifications << '\n';
}

} // namespace checkbox_example
