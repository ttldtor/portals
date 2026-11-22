// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/background.hpp>
#include <portals/detail/find_search.hpp>
#include <portals/ui_find_bar.hpp>
#include <portals/ui_scene.hpp>

namespace portals::detail {
struct FindPublication {
  std::shared_ptr<const FindResult> result;
  std::unique_ptr<GraphemeText> summary;
  std::size_t index{};
};

struct FindExternalNavigation {
  std::function<std::expected<FindMatch, TreeError>(FindDirection)> request;
};

struct FindRecord {
  WidgetTree *tree{};
  std::weak_ptr<const void> lifetime;
  Node<FindBar> field;
  Node<Widget> editor;
  Node<Label> summary;
  WidthPolicy policy{};
  FindLimits limits{};
  std::shared_ptr<const FindResult> result;
  std::unique_ptr<FindPublication> pending;
  std::shared_ptr<std::function<void(FindMatch)>> selected;
  std::size_t index{};
  bool active{true};
  bool busy{};
  bool readOnly{};
  bool dirty{};
  EditMode mode{};
  std::optional<TreeError> lastFailure;
  std::size_t preparedQueries{};
  std::shared_ptr<ui::detail::SceneActivity> activity;
  std::function<void(std::shared_ptr<const FindResult>)> requestModel;
  std::function<bool()> modelCurrent;
  bool modelPending{};
  std::unique_ptr<FindExternalNavigation> external;
};

struct FindAccess {
  // This private label always keeps its original policy and empty decorations. Commit only
  // prepared text by noexcept assignment: no value-parameter move construction or parsing.
  static std::expected<void, TreeError> installSummary(const std::shared_ptr<FindRecord> &record,
                                                       FindPublication &value) {
    std::expected<void, TreeError> result;
    auto edited = record->tree->edit(record->summary, [&](Label &label) {
      if (label.text_.policy() != value.summary->policy()) {
        result = std::unexpected(TreeError::INVALID_ARGUMENT);
        return;
      }
      if (label.text_ != *value.summary) {
        result = record->tree->invalidate(record->summary, Invalidation::LAYOUT);
        if (!result) {
          return;
        }
        label.text_ = std::move(*value.summary);
      }
      record->result = value.result;
      record->index = value.index;
    });
    return edited ? result : edited;
  }

  static FindMatch match(std::shared_ptr<const FindResult> result, std::size_t index) {
    FindMatch value;
    value.result_ = std::move(result);
    value.index_ = index;

    return value;
  }

  static bool current(const FindMatch &value, const std::shared_ptr<const FindResult> &result) noexcept {
    return value && value.result_ == result;
  }

  static const FindDocument &document(const FindMatch &value) noexcept {
    const auto &range = value.result_->ranges[value.index_];

    return *(range.document ? range.document : value.result_->document);
  }
};

struct FindExternalBinding {
  virtual ~FindExternalBinding() = default;
  virtual std::expected<std::shared_ptr<ui::detail::SceneActivity>, ManagedError>
  attach(ManagedScene &scene, const std::shared_ptr<FindRecord> &record) = 0;
};
} // namespace portals::detail

namespace portals::ui::detail {
std::expected<void, TreeError> publishFindResult(const std::shared_ptr<portals::detail::FindRecord> &record,
                                                 std::shared_ptr<const portals::detail::FindResult> result,
                                                 std::string_view notice, bool notify);
}
