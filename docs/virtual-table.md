# Show a virtual table

[Русская версия](ru/virtual-table.md)

Use [VirtualTable<T>](../include/portals/virtual_table.hpp). This fragment displays a Name header and three rows with standard keyboard and mouse navigation. Press F10 to close.

This fragment assumes `<portals/ui.hpp>` and `using namespace portals`; it uses Names, an immutable `ModelSnapshot<std::string>` adapter with three records, and displayText, which creates GraphemeText. The adapter, helper, headers and complete program appear below and in [guide_vtable.cpp](examples/guide_vtable.cpp).

```cpp
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
```

<details>
<summary>Complete program</summary>

```cpp
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
```

</details>

[Example source](examples/guide_vtable.cpp). Add the source to a C++23 project and link it with the portals::core target ([build instructions](getting-started.md)). Run the program in a terminal.

Each TableColumn owns a stable ColumnId, header, cell width and formatter. The recipe copies the column array, so it can be local to the factory. The scene handles preparation and input. Pending data requires prepared in ui::VirtualTableConfig<T>. TBD: the requested ui::vtable alias is unavailable; no delivery date is assigned.

## Read the visible rows

Keep a typed table reference and obtain its deferred visible source:

```cpp
ui::Ref<VirtualTable<std::string>> table;
auto visible = table.visibleModel();
```

Pass `table` to the reference overload of `ui::virtualTable`. Creating `visible`
does not read data; the table can be mounted later. In a UI event handler after
the table has been prepared, obtain one stable snapshot:

```cpp
auto rows = visible.snapshot();

if (!rows) {
  summary.text() = "Visible rows are not available";
  return;
}

std::size_t bytes = 0;

for (const auto &name : *rows) {
  bytes += name.size();
}

summary.text() = std::format("{} rows, {} UTF-8 bytes", rows->size(), bytes);
```

This fragment assumes `summary` is a mounted `ui::Ref<Label>` and the model row
type is `std::string`. Include `<format>` for `std::format`.

[`VisibleModelSnapshot<T>`](../include/portals/visible_model.hpp) is an owning,
read-only forward range of `const T&`. Its rows remain unchanged when the table
scrolls or is removed. `rows()` additionally exposes each value's `RowId`.
Copying a snapshot shares its immutable storage; it does not copy every value.
Retain the snapshot while using its iterators or references.

The range contains body rows intersecting the final viewport and ancestor clip,
including a partially visible row once. It excludes the header and overscan.
An empty ready table yields a successful empty range. Provider waiting/failure
and an unprepared view are distinct errors, not successful empty data.

<details>
<summary>Snapshot availability and lifetime</summary>

Call `snapshot()` on the owning UI thread, outside tree frames and managed
preparation callbacks. Before mount or after scene cleanup, access is closed;
removal preserves the original stale identity. A scroll/configuration change
can leave the view `NOT_PREPARED` until its next completed preparation/layout.
`PENDING` and `FAILED` describe provider readiness; failure text is owned.
`VisibleModelError` distinguishes these states from a `ManagedError` for access.
Checked reads return errors directly without automatic reporting.

A retained snapshot pins its provider storage, which can be larger than the
visible range. Avoid accumulating snapshots without a retention policy. Already
obtained snapshots stay readable after scene destruction; payload destruction
follows the model's existing thread contract. This API does not copy history or
add a streaming buffer.

</details>

Automatic computations and bindings over the visible source remain **TBD — M4**.

Terms: [recipe](glossary.md#glossary-recipe), [frame](glossary.md#glossary-frame), [ownership](glossary.md#glossary-ownership).

<details>
<summary>API reference and constraints</summary>

`VirtualTable<T>` presents immutable typed records through contiguous fixed-width
columns and fixed-height rows. The first cell row is always the header. A model
with no rows still displays its headers; an empty column set displays a blank
viewport. There are no row or cell widgets, editing controls, or implicit workers.

**Ownership and preparation**

Create the table on the UI owner with an independent `ModelConsumer<T>` from
`Model<T>::openRange()`, a span of `TableColumn<T>`, and `VirtualTableOptions`.
The constructor synchronously copies the definitions, headers, and formatter
captures. The span and its elements need live only until that call returns.
`T` need not be copyable or default-constructible: a formatter borrows `const T&`
from the immutable snapshot and returns an owning `GraphemeText`.

Each column has a unique nonzero `ColumnId` pair, positive width in cells,
header text, and a copyable formatter. Identity remains independent of visual
order; providers must increment the generation when reusing an identity.
Headers and formatter results use the options' width policy. Formatters must
not perform I/O, mutate the tree/model, keep row borrows, or allocate unbounded
temporary storage. Exceptions are translated at preparation, except that
termination inside a user callback cannot be recovered by the table.

Between frames, apply queued model publications, obtain one `ModelFrame<T>`,
and pass that same pin to `prepareVirtualTable` and sibling views. The local
viewport includes the header and must match the final arranged size exactly.
The application supplies this final size from its layout constraints. Preparation
must not run during `WidgetTree::frame`.

Preparation reads one viewport-plus-overscan range for all columns. Only ready
rows intersecting the body viewport and columns intersecting its width are
formatted. Overscan rows are requested but never formatted. The table stages
owning cell text, header text, stable IDs, and a copy of the pin together. A
formatter or allocation failure preserves the previous successful presentation.
A successful unchanged preparation does not invalidate or schedule another frame.
Updates outside the viewport may reprepare the bounded window; they never scan
the logical row count or locate IDs globally.

The table reuses VirtualList's checked window arithmetic and ancestor visibility
inspection. Its column clipping and header input geometry remain local to the
table; the range-request and navigation protocol follows VirtualList without a
new shared widget framework. See [the model contract](model.md) and
[VirtualList preparation](virtual-list.md).

A provider completion may contain a bounded prefetched page around the requested
interval. During later preparation the view can read the new exact window from
its retained completion snapshot when both data revision and row count match the
current pin. It retains one such owner, formats only current visible rows, and
respects the model's read budget. A cache miss remains pending and schedules the
new exact request; provider failures are not replaced by older ready data. A
revision change or hidden view releases the old prepared owner. Public base reads
and independent consumer results retain their existing exact semantics.

**Rendering, requests, and selection**

`measure`, `arrange`, and `paint` do no provider reads, formatting, mutation,
loading, or terminal I/O. Paint emits widget-local commands. The application
translates them through current node geometry and ancestor clipping, then renders
borrowed command text before the enclosing `tree.frame` returns. Each header and
cell has its own half-open clip; a wide grapheme never spills into the next column.
The last fixed-height row may be partially clipped. The body and header backgrounds
cover unused cells deterministically.

Pending cells show `...`; failed cells show `!`. Preparation returns each new
`ModelRequest<T>` once for caller dispatch and never starts a worker. Keep the
receipt until completion, or cancel it if dispatch fails. `retryVirtualTable`
requests a fresh receipt on the next preparation. Cancelled receipts still occupy
the model's in-flight budget until their owners release them. Hidden ancestors and
destruction cancel requests without joining workers; hidden preparation releases
the prepared range and text while retaining the selected stable ID.

`bindVirtualTable` uses the existing router for arrows, Home/End, left press, and
integral wheel movement. Keep the returned scoped `ActionBinding`; the tree/router
outlive it. A header click does not select a row. Body clicks account for surface
origin, clipping, header height, and fixed row height. Selection uses `RowId`, spans
the whole body row, and survives offscreen movement or reordering. Removed IDs
remain selected until explicitly cleared. Navigation searches only the prepared
visible IDs; pending positional intent is scoped to its data revision.

**Mutation, budgets, and errors**

Use the tree-checked free functions on the owner thread:

- `setVirtualTableFirst` sets an integral first row, cancels old work/intent, and
  invalidates paint; preparation clamps it to the model bounds.
- `setVirtualTableSelection` changes a stable ID and invalidates paint only.
- `setVirtualTableColumns` synchronously validates and copies a borrowed span.
  On validation, allocation, or capture-copy failure, the old configuration and
  presentation remain unchanged. On success, replacement invalidates layout and
  requires preparation before paint. The span is never retained.
- `setVirtualTableOptions` validates settings and existing header compatibility,
  invalidates layout, and requires preparation. To change the width policy,
  clear columns, set the new policy, then install rebuilt headers between frames.

Defaults bound configured columns to 64, visible body cells to 16384, and the
sum of all configured header UTF-8 bytes plus visible cell UTF-8 bytes to 1 MiB.
These are configurable positive limits, checked without silent truncation.
The existing model range limit independently bounds requested rows, including
overscan. The cell product is checked by division before multiplication could
overflow. Header validation is bounded by configured columns; no work scales with
logical row count. These bounds cover retained table data, not arbitrary temporary
allocations inside an application formatter.

Invalid dimensions, IDs, widths, policies, and exceeded budgets report
`TreeError::INVALID_ARGUMENT`; allocation failure reports `NO_MEMORY`, and other
formatter/capture-copy exceptions report `CALLBACK_FAILED`. Tree affinity,
lifetime, frame, and model-session errors stay explicit. Paint reports
`WOULD_BLOCK` for unprepared/dirty data and `INVALID_GEOMETRY` for a viewport
mismatch. Discard partial render commands on any paint/sink error. A setter can
make the old presentation dirty; a later failed preparation does not clear that
requirement.

</details>
