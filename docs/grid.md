# Arrange controls in a grid

[Русская версия](ru/grid.md)

Use [Grid](../include/portals/grid.hpp) when controls need shared columns or independent rows. `ui::grid` assigns cells in child order. Add `.columns(2)` to make a two-column grid; `.columnGap(2)` leaves two terminal cells between columns. The complete program below shows a name and value side by side. Press F10 to close.

```cpp
#include <portals/ui.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    auto scene = ui::scene(
      ui::grid(ui::label("Name"), ui::label("Ada"))
        .columns(2)
        .columnGap(2));

    return std::move(scene)
      | ui::onKey("F10", ui::quit);
  }
    | ui::runApp();
  // clang-format on
}
```

[Example source](examples/guide_grid.cpp). Add it to a C++23 project and link it with `portals::core` ([build instructions](getting-started.md)).

The declarative grid assigns cells automatically as children are added. Set `.row(...)` and `.column(...)` together when a control needs a specific position; `.rowSpan(...)` and `.columnSpan(...)` occupy multiple cells. Explicit and automatic placement can be combined. Placement is checked before controls are mounted, and overlapping rectangles are rejected. Hidden controls still reserve their declared cells.

## Header, body, and scrolling

Use `ui::header(...)` and `ui::body(...)` to create structural regions. They do not add wrapper controls: their children remain direct children of the grid. The regions share the same columns and column sizing, while each has its own row numbering and row sizing. Put all grid controls inside these sections when using regions.

Call `.sticky()` on the header to keep it at the top while the grid scrolls. Enable the grid's scroll owner with `.scroll()` and choose a fixed outer extent with `.height(...)` or `.width(...)` as needed. In the program below, the complete grid occupies three terminal rows; the header and any scroll chrome use part of that extent. Use the mouse wheel over the grid to scroll its body. F10 closes it.

```cpp
#include <portals/ui.hpp>
#include <utility>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    auto scene = ui::scene(
      ui::grid(
        ui::header(ui::label("Name"), ui::label("Role")).sticky(),
        ui::body(
          ui::label("Ada"), ui::label("Programmer"),
          ui::label("Grace"), ui::label("Admiral"),
          ui::label("Margaret"), ui::label("Engineer")))
        .columns(2)
        .columnGap(2)
        .scroll()
        .height(3));

    return std::move(scene)
      | ui::onKey("F10", ui::quit);
  }
    | ui::runApp();
  // clang-format on
}
```

[Complete regions example](examples/guide_grid_regions.cpp). A fixed height limits the visible scroll area; it does not virtualize rows. Grid children are materialized controls, including children outside the current viewport.

## Track sizes and presentation

Use `.columnSizes(...)` to set policies for all resolved shared columns. Use `.rowSizes(...)` for body rows; header rows keep their own inferred policies. Each policy list must match its resolved track count. `.rows(...)` sets the body row count in column-major flow; `.rowSizes(...)` only sets sizes and does not set that count.

The C++ API can set header, body, and alternating body-row backgrounds through `GridPresentation`, for example with `.presentation(...)`. Backgrounds are painted below real controls and do not become control styles. Controls with opaque backgrounds can cover them. The scene-resource format supports structural `header` and `body` sections and their placement properties, but does not currently express `CellStyle` presentation values.

## Scene resources

Loaded grids use the same checked automatic placement and region rules. This fragment shows the supported `Grid` properties and section structure:

```text
Grid#grid { columns=2
  header { sticky=true Label#heading { text="Heading" columnSpan=2 } Label#sub { text="Sub" } }
  body { Label#first { text="a" } Label#second { text="b" row=0 column=1 } }
}
```

The `header` and `body` sections are structural; their children are ordinary controls. Resource properties do not provide the C++ `GridPresentation`/`CellStyle` API.

## Explicit low-level configuration

`GridOptions` remains available when a caller needs exact low-level placement. It owns nonempty `columns` and `rows` policies plus `GridCell` entries keyed by each child's full `NodeId`. Call `setGridOptions` after the nodes exist and before the next layout. This explicit mode does not infer tracks or placements and continues to allow overlapping cells in sibling order. The declarative `ui::grid` API is additive; it does not change this contract. `Overlay` remains available when every child should share one content rectangle.

Declarative placement defaults to at most 4096 tracks per axis, 4096 structural children, and 65536 units of placement work. Adjust these limits with `.gridOptions(DeclarativeGridOptions{...})`. C++ `GridPresentation` has a separate positive `maxPaintCells` budget for backgrounds and scroll chrome, defaulting to 16384. Invalid placement or track settings return `INVALID_ARGUMENT`; exhausted placement limits return `NODE_LIMIT`; coordinate overflow returns `INVALID_GEOMETRY`; allocation failure returns `NO_MEMORY`. Painting above `maxPaintCells` returns `WOULD_BLOCK` before submitting commands.

See [linear layouts](layout.md), [geometry](geometry.md), and [the frame ownership contract](widget-tree.md).
