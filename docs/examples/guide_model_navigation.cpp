// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <array>
#include <string_view>

using namespace portals;

// Static names outlive every snapshot; views borrow no mutable or temporary strings.
class Cities final : public ModelSnapshot<std::string_view> {
public:
  explicit Cities(bool reverse = false) : ModelSnapshot(1, NAMES.size()), reverse_(reverse) {
    for (std::size_t index = 0; index < rows_.size(); ++index) {
      const auto original = reverse ? rows_.size() - index - 1 : index;

      rows_[index] = {{original + 1, 1}, NAMES[original]};
    }
  }

  ModelRows<std::string_view> read(RowRange range) const override {
    return {RangeState::READY, std::span(rows_).subspan(range.first, range.count), {}};
  }

  RowLookup locateRow(RowId id) const override {
    if (id.generation != 1 || id.value == 0 || id.value > rows_.size()) {
      return {RowLookupState::NOT_FOUND, 0};
    }

    return {RowLookupState::FOUND, reverse_ ? rows_.size() - id.value : id.value - 1};
  }

private:
  static constexpr std::array<std::string_view, 12> NAMES{"Paris",  "Prague",   "London", "Tokyo",  "Oslo", "Berlin",
                                                          "Vienna", "Helsinki", "Madrid", "Lisbon", "Rome", "Tallinn"};
  std::array<ModelRow<std::string_view>, NAMES.size()> rows_;
  bool reverse_{};
};

GraphemeText cityCaption(const ModelRow<std::string_view> &row) {
  return GraphemeText::from(row.value).value();
}

/** @brief Share a current record across three views and reading anchors across two.
 * Tab changes focus, arrows change current, wheel scrolling changes reading only, F10 exits.
 * Provider setup uses the checked managed API; the scene owns models and navigation.
 */
int main() {
  ui::Ref<VirtualList<std::string_view>> cities;
  ui::Ref<VirtualList<std::string_view>> mirror;
  ui::Ref<VirtualList<std::string_view>> reversed;
  ManagedModel<std::string_view> model;

  // clang-format off
  auto scene = ui::scene([&](ManagedScene &owner) {
    return owner.model<std::string_view>(std::make_shared<Cities>()).and_then([&](auto source) {
      model = source;

      return owner.model<std::string_view>(std::make_shared<Cities>(true)).transform([&](auto reverse) {
        return ui::vbox(
          ui::label("Shared cities | Tab: focus | arrows: current | wheel: read | F10: quit"),
          ui::hbox(
            ui::virtualList(cities, source, cityCaption).width(20).height(7).framed("rounded", "Cities"),
            ui::virtualList(mirror, source, cityCaption).width(20).height(7).framed("rounded", "Same order"),
            ui::virtualList(reversed, reverse, cityCaption).width(20).height(7).framed("rounded", "Reverse order")),
          ui::label("Current is shared. Scrolling the first two views shares their reading position."));
      });
    });
  });

  return scene
    | ui::configure([&](ManagedScene &owner) -> std::expected<void, ManagedError> {
        auto navigation = owner.navigation(model);

        if (!navigation) {
          return std::unexpected(navigation.error());
        }

        return owner.bindNavigation(cities.node(), *navigation, {.readingAnchor = true})
          .and_then([&] {
            return owner.bindNavigation(mirror.node(), *navigation, {.readingAnchor = true});
          })
          .and_then([&] {
            return owner.bindNavigation(reversed.node(), *navigation,
              {.projection = NavigationProjection::PRESERVES_ROW_IDS});
          });
      })
    | ui::theme(nordTheme())
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
