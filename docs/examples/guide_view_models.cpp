// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/table_column.hpp>
#include <portals/text_format.hpp>
#include <portals/ui.hpp>

#include <array>
#include <string_view>

using namespace portals;

struct City {
  std::string_view name;
  int orders;
};

class Cities final : public ModelSnapshot<City> {
public:
  explicit Cities(bool asia) : ModelSnapshot(1, 12) {
    constexpr std::array EUROPE{"Prague", "Vienna", "Berlin", "Paris", "Rome",    "Oslo",
                                "Madrid", "Lisbon", "Warsaw", "Riga",  "Tallinn", "Helsinki"};
    constexpr std::array ASIA{"Tokyo",  "Seoul", "Osaka",  "Taipei", "Kyoto",   "Busan",
                              "Nagoya", "Kobe",  "Sendai", "Nara",   "Fukuoka", "Sapporo"};
    for (std::size_t index = 0; index < rows_.size(); ++index) {
      rows_[index] = {{index + 1, 1}, {asia ? ASIA[index] : EUROPE[index], static_cast<int>(index + 1) * 3}};
    }
  }

  ModelRows<City> read(RowRange range) const override {
    return {RangeState::READY, std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

  RowLookup locateRow(RowId id) const override {
    return id.generation == 1 && id.value && id.value <= rows_.size() ? RowLookup{RowLookupState::FOUND, id.value - 1}
                                                                      : RowLookup{RowLookupState::NOT_FOUND, 0};
  }

private:
  std::array<ModelRow<City>, 12> rows_;
};

int main() {
  ui::Ref<VirtualList<City>> list;
  ui::Ref<VirtualTable<City>> table;
  ui::Ref<Label> region;
  std::array<ManagedModel<City>, 2> models;
  ManagedNavigation navigation;

  // clang-format off
  auto scene = ui::scene([&](ManagedScene &owner) -> std::expected<ui::Description<Column>, ManagedError> {
    for (std::size_t index = 0; index < models.size(); ++index) {
      auto created = owner.model<City>(std::make_shared<Cities>(index == 1));
      if (!created) {
        return std::unexpected(created.error());
      }
      models[index] = *created;
    }
    auto columns = tableColumns(
      tableColumn("City", &City::name, cellFormat<std::string_view>("{}"), 18),
      tableColumn("Orders", &City::orders, cellFormat<int>("{}"), 8));
    if (!columns) {
      return std::unexpected(ManagedError{.phase = ManagedPhase::MOUNT,
        .cause = TreeError::INVALID_ARGUMENT, .application = {}, .cleanup = {}, .text = columns.error()});
    }
    return ui::vbox(
      ui::label(region, "Europe | F2: Europe | F3: Asia | F10: quit"),
      ui::hbox(
        ui::virtualList(list, models[0], [format = cellFormat<std::string_view>("{}")](const ModelRow<City> &row) {
          return format(row.value.name);
        }).width(22).framed("rounded", "Cities"),
        ui::virtualTable(table, models[0], *columns).framed("rounded", "Orders")).height(8),
      ui::label("Tab: focus | arrows: current | both models remember their own place"));
  });

  return scene
    | ui::configure([&](ManagedScene &owner) -> std::expected<void, ManagedError> {
        auto created = owner.navigation(models[0]);
        if (!created) {
          return std::unexpected(created.error());
        }
        navigation = *created;
        auto bound = owner.bindNavigation(list.node(), navigation, {.readingAnchor = true});
        if (!bound) {
          return bound;
        }
        return owner.bindNavigation(table.node(), navigation, {.readingAnchor = true});
      })
    | ui::theme(nordTheme())
    | ui::onKey("F2 | F3", [&](const KeyEvent &event) {
        const auto index = event.key == InputKey::F2 ? 0u : 1u;
        if (navigation.model(models[index])) {
          region.text() = index == 0 ? "Europe | F2: Europe | F3: Asia | F10: quit"
                                    : "Asia | F2: Europe | F3: Asia | F10: quit";
        }
      })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
