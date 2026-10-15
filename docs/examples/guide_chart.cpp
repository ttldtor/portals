#include <portals/ui.hpp>
#include <array>

using namespace portals;

class Prices final : public ModelSnapshot<Ohlc> {
public:
  Prices() : ModelSnapshot(1, 4) {
  }

  ModelRows<Ohlc> read(RowRange range) const override {
    return {RangeState::READY,
            std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

private:
  const std::array<ModelRow<Ohlc>, 4> rows_{{
    {{1, 1}, {10, 14, 9, 12}}, {{2, 1}, {12, 16, 11, 15}},
    {{3, 1}, {15, 15, 10, 11}}, {{4, 1}, {11, 14, 10, 13}}
  }};
};

int main() {
  auto scene = ui::scene([](ManagedScene &owner) {
    return owner.model<Ohlc>(std::make_shared<Prices>()).transform([](auto model) {
      return ui::chart(model, [](const Ohlc &price) {
        return std::optional{price};
      }, ui::ChartConfig<Ohlc>{.options = {.mode = ChartMode::LINE}});
    });
  });

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
