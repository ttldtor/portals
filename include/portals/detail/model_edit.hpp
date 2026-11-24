// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <limits>
#include <portals/form_field.hpp>
#include <portals/managed_navigation.hpp>
#include <portals/model_edit.hpp>

namespace portals::detail {
/// @cond INTERNAL
struct ManagedModelSource;

/** Session-owned storage used by the existing LineEdit algorithms. */
struct EditTextDraft {
  explicit EditTextDraft(GraphemeText value, const LineEditOptions &options)
      : text(std::move(value)),
        history(std::make_unique<TextHistory>(options.history, options.widthPolicy, options.limits)),
        limits(options.limits) {
  }

  GraphemeText text;
  std::unique_ptr<GraphemeText> mask;
  std::unique_ptr<TextHistory> history;
  TextLimits limits;
  std::size_t cursor{};
  std::size_t anchor{};
  std::int32_t scroll{};
  std::uint64_t revision{};
  std::uint64_t generation{};
  bool initialized{};
  bool dirty{};
};

/** Exact owning text-buffer capacities for the optional edit-session budget. */
struct EditStorageAccess {
  static std::expected<std::size_t, TreeError> draftBytes(const GraphemeText &text, const GraphemeText *mask,
                                                          std::size_t history) noexcept {
    auto total = sizeof(EditTextDraft) + sizeof(TextHistory);
    auto add = [&](std::size_t bytes) {
      if (bytes > std::numeric_limits<std::size_t>::max() - total) {
        return false;
      }
      total += bytes;
      return true;
    };
    auto payload = [&](const GraphemeText &value) {
      return value.graphemes_.capacity() <= std::numeric_limits<std::size_t>::max() / sizeof(Grapheme) &&
             add(value.bytes_.capacity()) && add(1) && add(value.graphemes_.capacity() * sizeof(Grapheme));
    };
    if (!payload(text) || (mask && (!add(sizeof(GraphemeText)) || !payload(*mask))) || !add(history)) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    return total;
  }
};

// Preserve a typed scalar conversion error across the existing bool validation callback.
struct EditTextFailure {
  TreeError error;
};

/** Optional bridge; its owning closures retain the field session, never a scene. */
struct EditTextBinding {
  EditTextDraft *displayed{};
  EditTextDraft *prepared{};
  void *displayedEntry{};
  void *preparedEntry{};
  LineEditStatus publicationStatus{LineEditStatus::FAILED};
  bool validatedByEditor{};
  bool asynchronous{};
  const void *definition{};
  bool readOnly{};
  bool writable{};
  bool retentionRefused{};
  bool closed{};
  std::function<std::expected<std::shared_ptr<const FieldValidation>, TreeError>()> validation;
  std::function<std::expected<void, TreeError>(std::shared_ptr<const FieldValidation>)> displayValidation;
  std::function<std::expected<void, TreeError>()> preserveValidation;
  std::function<std::expected<bool, TreeError>(std::string_view)> checkCandidate;
  std::function<std::expected<bool, TreeError>(std::string_view)> validateBeforeParse;
  std::function<std::expected<bool, TreeError>(std::string_view)> validateForCommit;
  std::function<std::expected<void, TreeError>(const LineEditOptions &)> initialize;
  std::function<std::expected<void, TreeError>()> prepare;
  std::function<std::expected<void, TreeError>(const NavigationPosition &, const std::shared_ptr<ManagedModelSource> &)>
    preflight;
  std::function<std::expected<std::shared_ptr<const PropertyChange<std::string>>, TreeError>()> snapshot;
  std::function<std::expected<std::shared_ptr<const PropertyChange<std::string>>, TreeError>()> cancelSnapshot;
  std::function<std::expected<std::optional<PropertyUpdate<std::string>>, TreeError>(const std::string &)> publish;
  std::function<std::expected<void, TreeError>(std::size_t, bool)> admit;
  std::function<void()> accepted;
  std::function<std::expected<Subscription, TreeError>(Node<LineEdit>, std::function<void()>)> observe;
  std::function<std::expected<LineEditResult, TreeError>(bool)> complete;
  std::function<std::expected<LineEditResult, TreeError>(const std::string &)> setText;
};

struct EditNavigationParticipant {
  virtual ~EditNavigationParticipant() = default;
  virtual std::expected<void, TreeError> beforeTransition(const NavigationPosition *,
                                                          const std::shared_ptr<ManagedModelSource> &) = 0;
  virtual void closed() noexcept = 0;
};

/// @endcond
} // namespace portals::detail
