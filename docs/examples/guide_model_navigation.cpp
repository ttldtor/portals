// Copyright (c) 2026 ttldtor.

// SPDX-License-Identifier: BSL-1.0

#include <portals/ui.hpp>

#include <array>
#include <functional>

#include <string_view>
#include <utility>

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

/** @param name One of this sample's fixed valid ASCII city names.
 * @return Owning grapheme text without applying another format pattern.
 */
GraphemeText cityCaption(std::string_view name) {
  return GraphemeText::from(name).value();
}

/** @brief Reuse the city's formatter and viewport across differently ordered models.
 * @tparam Source Cold model or navigation descriptor of city-name rows.
 * @param source Owning declaration copied into the returned view recipe.
 * @return Ordinary single-use list description; callers choose its title and observation.
 */
template <class Source> auto cityList(Source source) {
  return ui::virtualList(std::move(source), std::identity{}, cityCaption).width(20).height(7);
}

/** @brief Share a current record across three views and reading anchors across two.
 * Tab changes focus, arrows change current, wheel scrolling changes reading only, F10 exits.
 */

int main() {

  ui::Navigation<std::string_view> navigation;

  ui::Ref<VirtualList<std::string_view>> reversed;

  // clang-format off
  return ui::scene([&](ManagedScene &) {
    auto source = ui::model<std::string_view>(std::make_shared<Cities>());
    auto reverse = ui::model<std::string_view>(std::make_shared<Cities>(true));

    navigation = ui::navigation(source, {.readingAnchor = true});

    return ui::vbox(
      ui::label("Shared cities | Tab: focus | arrows: current | wheel: read | F10: quit"),
      ui::hbox(
        cityList(navigation).framed("rounded", "Cities"),
        cityList(navigation).framed("rounded", "Same order"),
        cityList(reverse).reference(reversed).framed("rounded", "Reverse order")),
      ui::label("Current is shared. Scrolling the first two views shares their reading position."));
  })
    | ui::configure([&navigation, reversed](ManagedScene &owner) -> std::expected<void, ManagedError> {
        auto managed = navigation.managed();

        if (!managed) {
          return std::unexpected(managed.error());
        }

        return owner.bindNavigation(reversed.node(), *managed,
          {.projection = NavigationProjection::PRESERVES_ROW_IDS});
      })
    | ui::theme(nordTheme())
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();

  // clang-format on
}
