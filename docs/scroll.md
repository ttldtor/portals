# Scroll and keep the viewport position

[Русская версия](ru/scroll.md)

Use [ScrollState](../include/portals/scroll.hpp). Lists and tables each keep their own logical row and fractional remainder. This fragment supports wheel and keyboard scrolling. Resize the terminal to reduce the visible area. Press F10 to close.

This fragment assumes `<portals/ui.hpp>` and `using namespace portals`; it uses Names, an immutable `ModelSnapshot<std::string>` adapter with three records, and displayText, which creates GraphemeText. The adapter, helper, headers and complete program appear below and in [guide_scroll.cpp](examples/guide_scroll.cpp).

```cpp
int main() {
  auto scene = ui::scene([](ManagedScene &owner) {
    return owner.model<std::string>(std::make_shared<Names>()).transform([](auto model) {
      return ui::virtualList(model, [](const ModelRow<std::string> &row) {
        return displayText(row.value);
      });
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
      return ui::virtualList(model, [](const ModelRow<std::string> &row) {
        return displayText(row.value);
      });
    });
  });

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

</details>

[Example source](examples/guide_scroll.cpp). Add the source to a C++23 project and link it with the portals::core target ([build instructions](getting-started.md)). Run the program in a terminal.

A live view keeps position across frames and hiding. A provider with bounded locateRow support also preserves the first visible record across insertions. Restart or rebuilding the view requires application-owned storage. Automatic persistent scroll storage is TBD; no delivery date is assigned.

Terms: [recipe](glossary.md#glossary-recipe), [frame](glossary.md#glossary-frame), [ownership](glossary.md#glossary-ownership).

## Restore after rebuilding a view

This fragment runs on UI between frames. tree is a live tree, list is its Node<VirtualList<std::string>>, savedRow is application-owned uint64_t storage, and reportError is the application error handler. Read before removing the old list; restore after constructing the replacement.

```cpp
auto saved = tree.read(list, [&](const VirtualList<std::string> &view) {
  savedRow = view.firstRow();
});

if (!saved) {
  reportError(saved.error());
}

// Later, after rebuilding list:
auto restored = setVirtualListFirst(tree, list, savedRow);

if (!restored) {
  reportError(restored.error());
}
```

For a table, use VirtualTable<T>::firstRow and setVirtualTableFirst. The next preparation clamps the position to the current row count. The integral setter resets the fractional remainder and identity anchor. scrollState() returns a copy including the fraction, but views expose no setter restoring that entire copy: fractional and identity restoration is TBD, with no delivery date. The application chooses its file format and save point; widgets perform no I/O.

<details>
<summary>API reference and constraints</summary>

`<portals/scroll.hpp>` supplies `ScrollState`, which is also the scroll state owned
by `VirtualList<T>` and `VirtualTable<T>`. All view mutations use the existing UI
owner and scheduler. No second event loop, worker or provider scan is introduced.

**Logical position and raster position**

The logical position is the pair `(rasterOffset(), fraction())`, in logical rows.
The first component is an exact `uint64_t`; the second is a finite `double` in
`[0, 1)`. The raster policy is floor quantization. Four movements of `0.25` advance
one row; moving `-0.25` from row 1 produces row 0 plus fraction `0.75`. Text starts
on integral cell coordinates; `row_height` converts logical rows into cell rows.
Floating-point fractions retain normal binary rounding limitations. A rounded
borrowed remainder of 1 is reduced to the nearest representable value below 1.

`ScrollState::moveBy(rows, count)` accepts finite signed motion and saturates at
row 0 and `count - 1`. Overscroll at either boundary is discarded. An empty model
resets both parts. The integer component never passes through `double`, including
indices above 2^53. Invalid fractions and nonfinite movement return
`TreeError::INVALID_ARGUMENT` without changing state. No allocation or callbacks
occur in this value type; standalone owners must serialize mutation.

After the first successful preparation, a precision input adapter can call scrollVirtualList(tree, list, 0.25) between frames on UI; the amount is in logical rows.

`scrollVirtualTable` is equivalent. Native integral wheel events use the same
`ScrollState`. Device-specific scaling stays in the input adapter. Fraction-only
changes neither invalidate paint nor cancel requests, read providers, or format
rows. Raster changes invalidate paint and require another `prepareVirtualList`
or `prepareVirtualTable` before painting. The caller still dispatches any request
receipt returned by preparation. `setVirtualListFirst` and `setVirtualTableFirst`
remain exact integral positioning operations; they reset fractional motion and
replace any old anchor intent.

**Stable identity and ensure-visible**

Providers can override `ModelSnapshot<T>::locateRow(RowId)`. Use an identity index
associated with that immutable snapshot, with bounded UI work (for example an
existing hash index or ordered index). Do not scan the full model, load data,
block, or access a newer snapshot from this method. The full `RowId`, including
generation, determines identity. The owning result has three explicit states:

- `FOUND`: `index` belongs to this snapshot and is less than its row count.
- `NOT_FOUND`: lookup is supported, but that identity is absent.
- `UNSUPPORTED`: the default for existing providers without an identity index.

`ModelFrame<T>::locateRow` uses its retained base snapshot, validates identities
and result bounds, and translates allocation/other exceptions to `NO_MEMORY` and
`CALLBACK_FAILED`. It performs no request dispatch or invalidation.

The first ready visible row becomes the viewport anchor, independent of selection.
At a new data revision, capable providers relocate this identity before bounded
range preparation. Insertion or deletion before the viewport therefore preserves
the anchored row and fractional remainder. If the anchor itself is deleted, the
view keeps the previous ordinal clamped to the new last row; the next ready first
row becomes the new anchor. If lookup is unsupported, the same ordinal fallback
preserves legacy behavior: identity anchoring is not promised for that provider.
Lookup/formatting failure leaves the previous prepared presentation intact.

Use `ensureVirtualListVisible(tree, list, pin, id)` or
`ensureVirtualTableVisible(tree, table, pin, id)` with the same shared application
pin used for the next preparation. The result distinguishes all three lookup
states. `FOUND` moves the position minimally using the last prepared viewport,
invalidates paint and cancels previous range work; selection is unchanged. A
table's fixed header is excluded from body capacity. Rows taller than a nonempty
viewport are revealed at their leading cell. A zero-height viewport cannot
reveal a row. Prepare again, dispatch returned work if needed, then paint the same
pin with sibling views. Before initial preparation, scroll/ensure operations
return `WOULD_BLOCK`; a foreign model pin or stale node is rejected by existing
checked handles. Read getters through `tree.read`; mutate only on the UI owner,
outside `tree.frame`. No borrowed row or widget pointer escapes these operations.

**Culling without stopping the model**

Hidden ancestors release prepared list/table rows and cancel their range requests.
The stable anchor and selection survive hiding. Model publication continues through
the existing scheduler; showing the view again prepares the current shared pin and
relocates its anchor. Hidden preparation performs no formatting or provider reads.

`WidgetTree::paint` skips a widget when an ancestor is hidden or valid geometry has
an empty clip. An offscreen paint hook is therefore not called at all. Invalid
geometry still reports `INVALID_GEOMETRY`; it is not treated as an empty clip.
This does not stop model publication or change the layout/preparation lifecycle.
Overscan rows may be requested by views, but only visible logical rows are formatted.

</details>
