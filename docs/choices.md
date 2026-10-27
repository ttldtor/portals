# Model-backed choices {#choices}

[Русская версия](ru/choices.md) · [Models](model.md) · [LineEdit](line-edit.md) · [Live suggestions](live-search.md)

Use `ComboBox` for an editable draft with a choice from a source. Use `DropDown`
for a read-only main display. Both accept immutable models and load visible popup
rows on workers through the existing virtual-list provider.

![The Shipping desk example with destination, delivery and palette controls.](images/choices.png)

The complete Shipping desk application also demonstrates list navigation, filtered
selection and dark/light palette changes:

![Native Shipping desk session browsing cities, selecting a result and changing palette.](images/choices.gif)

These images show a captured native application rendered by VHS. Appearance can
vary with terminal fonts and capabilities.

## A complete application

[Full source](examples/guide_choices.cpp)

\include guide_choices.cpp

Type a city, press Down to open the popup and Enter to accept a highlighted row.
Escape dismisses the popup. F10 exits through the ordinary runner. The factory
copies the small literal source; references and callbacks require no manual
binding-token storage.

Build `portals_docs_choices` with tests enabled, or build
`portals_choices_example` with examples enabled for the larger form.
The [build guide](getting-started.md) describes the native presets.

## Prefix and fuzzy matching

`ComboBox` and `DropDown` use `SearchStrategy::PREFIX` by default: matching is
case-insensitive and accent-insensitive over the normalized label or a whitespace
token. With the catalog below, `Pra` shows only Prague; Bratislava, Paris and
São Paulo are excluded. `Pr2` has no exact prefix match.

Use `.fuzzy()` when an application should tolerate one insertion, deletion or
substitution from three normalized query graphemes. This complete example also
shows Bratislava for `Pra`:

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;
  auto scene = ui::scene(ui::comboBox({"Prague", "Bratislava", "Paris", "São Paulo"}).fuzzy());
  return scene | ui::onKey(key::F10, ui::quit) | ui::runApp();
}
```

Use `.prefix()` to restore strict matching after a fuzzy setting:

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;
  auto scene = ui::scene(ui::comboBox({"Prague", "Bratislava", "Paris", "São Paulo"}).fuzzy().prefix());
  return scene | ui::onKey(key::F10, ui::quit) | ui::runApp();
}
```

The pipe forms are `recipe | ui::fuzzy()` and `recipe | ui::prefix()`;
`.fuzzy(false)` also selects PREFIX. These modifiers override only the strategy,
retaining every configured worker budget. `ComboBoxConfig::search.strategy`
remains available for explicit configuration. Sparse `.settings()` patches and
fluent modifiers follow the existing last-value rule for the same leaf.
General `SearchOptions` and live suggestions retain their FUZZY_PREFIX default.

## Draft and accepted selection

The main editor and accepted selection represent different state:

- Typing changes the ordinary LineEdit draft and follows its configured editing,
  validation and Undo rules. Typing leaves the source catalog unchanged.
- A highlighted row is a navigation position. Enter or a pointer activation
  accepts it and updates the editor through its normal checked command path.
- `ChoiceSelection` owns a bounded label and source-row identity. Duplicate labels
  remain distinct rows. `text()` remains readable after the source or control is
  retired; currentness is checked before a saved handle can be accepted again.
- `ui::onSelect(ref, callback)` reports accepted-selection changes, including an
  empty handle after clearing or source invalidation. Repeating the same accepted
  value or clearing an already empty selection produces no extra notification.
- `clearSelection()` clears the accepted row while retaining the displayed text,
  including a DropDown caption. Inspect `selection()` or `onSelect` to distinguish
  an accepted row from retained text. Escape on an open popup dismisses it while preserving already applied immediate edits.

`combo.text()` exposes the standard editable text property. `combo.editor()`
provides advanced access to its LineEdit. A DropDown's main display is read-only; its optional popup search field has its own transient query.
Closing a ComboBox popup preserves its main draft and editor history.

## Models and projections

A source can be a managed model, an owning immutable snapshot, or a
`RowCollection<T>` including its checked construction result. For example, given
an existing model of `City` rows, the projection selects the displayed field:

```cpp
auto input = ui::comboBox(city, cities, &City::name);
```

The projection runs on a worker and must read immutable data safely. Keep UI
references and thread-affine services out of that projection. Source changes
invalidate incompatible accepted rows and pending completions while retaining
visible draft text. Original source ordinals remain distinct from ranked-search
positions.

For startup selection, set `ComboBoxConfig::initial`. A programmatic `select()`
over a managed model requires the first ordinary preparation cycle to have pinned
that model. `runApp` drives the normal preparation cycles; low-level hosts call
`ManagedScene::prepare` themselves. `Scene::prepareChecked()` mounts the scene
but does not perform this runtime model preparation.

A projection can also return an owning `std::string`, for example from
`std::format`. The worker pins that result while preparing display text.
View-returning projections borrow the immutable source snapshot. Row-byte and
scratch budgets bound library-owned storage; allocations made inside an
application projection before it returns remain the application's responsibility.

The ordinary browse list can navigate the complete source using bounded visible
requests. A nonempty search query uses the bounded indexed-search path described
in [live suggestions](live-search.md); its top-K limit is a search-result limit.

## Programmatic commands

`open()`, `select(index)` and `select(selection)` return checked admission
results. Popup creation,
layout and source-row resolution complete through existing UI/worker phases.
An admitted request has not necessarily opened a popup or accepted a row yet.
`opened()` observes the committed state; `onSelect` reports the eventual accepted
selection. Newest intent and exact source identities fence stale work.

`select(index)` uses a zero-based source ordinal captured for that request.
`select(selection)` schedules a still-current owning selection through the same
deferred command path, including calls from button callbacks. `dismiss()` also
cancels a pending open. Closing or removing the owner retires its transient popup
and popup-owned result models. An explicit programmatic selection can resolve its
one requested row while the popup is closed.

Provider and transport failures enter the existing typed diagnostics and runner
error policy. The optional failure callback can provide immediate application
feedback. Recoverable action diagnostics follow the [runner contract](application.md).

## Presentation and limits

Use natural `ComboBox` and `DropDown` selectors and the existing theme/style
layers. The Shipping desk form retains its application rules while changing the
base theme. Popup placement flips or clamps to the terminal viewport.

The scene paints the theme background across the viewport, including layout
gaps and unused space. Changing the palette updates this background together
with the controls. No additional background widget is required. An application
rule such as `Scene { background: #182028; }` overrides the theme background and
remains in effect when the theme changes.

`ComboBoxConfig` separates the visible popup height, width, visible-range budget
and search limits. Defaults include eight visible rows, a 60-cell maximum popup
width, a 32-row browse-request bound and the existing indexed-search budgets with PREFIX matching.
Tune these only when the application needs different limits; invalid settings
report checked errors.

The scene supplies the existing overlay host automatically when required and
reuses a compatible explicit host. A popup occupies its normal layer/model
resources while open and releases them when dismissed. Closed controls perform
no autonomous browse or search. The main retained controls and their draft state
remain in the tree.

The main controls use the natural PSS types `ComboBox` and `DropDown`. Their
children have `.choice-editor` and `.choice-trigger` classes. The popup uses
`Dialog.choice-popup`, with `.choice-search`, `.choice-list` and
`.choice-completion` children. Existing LineEdit, Button, VirtualList and Dialog
default styles supply the skin; no per-control registration is needed.

## Full form and customization

[Complete Shipping desk source](../examples/choices/main.cpp)

\include choices/main.cpp
## Loaded scenes

The resource registry recognizes `ComboBox` and `DropDown` and their ordinary
control settings, including `text`, `placeholder` and
`searchStrategy=PREFIX` or `searchStrategy=FUZZY_PREFIX`. Source models and callbacks
remain C++ declarations. To supply a catalog from a resource, register a typed
factory with `SceneResourceOptions::control<ComboBox>` or
`control<DropDown>` and return the corresponding recipe with
`.settings(arguments.settings)`. The [scene-loading guide](scene-loading-guide.md)
shows the registration and lazy runner error path.

Typed factories and aliases advertise their popup-host requirement without
running application callbacks twice. An opaque `Widget` factory that conditionally
returns a choice must use an explicit compatible `ui::overlayHost`.

Protected `PASSWORD` and `NO_ECHO` editor modes are rejected with
`INVALID_ARGUMENT` before choice references publish: catalog search must not
send protected editor contents to a worker projection.
