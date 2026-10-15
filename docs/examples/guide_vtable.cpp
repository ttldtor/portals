#include <portals/ui.hpp>
#include <array>
#include <stdexcept>

using namespace portals;

class Names final : public ModelSnapshot<std::string> {
public:
  Names() : ModelSnapshot(1, 3) {
  }

  ModelRows<std::string> read(RowRange range) const override {
    return {RangeState::READY,
            std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

private:
  const std::array<ModelRow<std::string>, 3> rows_{{
    {{1, 1}, "Ada"}, {{2, 1}, "Grace"}, {{3, 1}, "Margaret"}
  }};
};

GraphemeText displayText(std::string_view value) {
  auto result = GraphemeText::from(value);

  if (!result) {
    throw std::runtime_error("Invalid display text");
  }

  return std::move(*result);
}

int main() {
  auto scene = ui::scene([](ManagedScene &owner) {
    return owner.model<std::string>(std::make_shared<Names>()).transform([](auto model) {
      const std::array<TableColumn<std::string>, 1> columns{{
        {{1, 1}, displayText("Name"), 18, [](const std::string &name) {
          return displayText(name);
        }}
      }};

      return ui::virtualTable(model, columns);
    });
  });

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}

