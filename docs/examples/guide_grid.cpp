#include <portals/ui.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<Grid> grid;
    ui::Ref<Label> name;
    ui::Ref<Label> value;

    auto scene = ui::scene(ui::grid(grid,
      ui::label("Name").reference(name),
      ui::label("Ada").reference(value)));

    return std::move(scene)
      | ui::configure([=](ManagedScene &, Application &app) -> std::expected<void, ManagedError> {
          GridOptions options;

          options.columns = {AxisSize{}, AxisSize{}};
          options.rows = {AxisSize{}};
          options.column_gap = 2;
          options.cells = {
            {name.node().id(), 0, 0},
            {value.node().id(), 0, 1}
          };

          auto configured = setGridOptions(app.tree(), grid.node(), options);

          if (!configured) {
            return std::unexpected(
              ManagedError{ManagedPhase::BIND, grid.node().id(), configured.error(), {}, {}});
          }

          return {};
        })
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
