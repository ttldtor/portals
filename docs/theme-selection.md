# Checked theme previews {#theme-selection}

[Русская версия](ru/theme-selection.md)

The theme-selection model lets an application preview a catalog choice, commit it,
or restore the previous theme. It uses the existing style engine and preserves
application and user overrides. This page covers the checked model API. For a
ready-made list and navigation, use the [theme selector](theme-selector.md).

## Complete native example

The maintained example provides explicit preview, apply and cancel buttons.
Apply requires a successful preview. Its handlers return checked results to the
ordinary runner, so an unhandled failure follows the application's error policy.
Use this customization example when the application supplies its own controls.

\include{lineno} examples/guide_theme_selection.cpp

The same [complete source](examples/guide_theme_selection.cpp) is registered for
the native `portals_docs_theme_selection` target and the installed-package consumer. Replay transport
and failure injection belong to the tests, not this native application.

## Setup and ownership

`ui::themeSelection(reference, choices, options)` is a deferred scene stage.
Each `ui::ThemeChoice` owns its nonempty application ID, display text and factory.
A factory returns a `StyleSheet` or exactly `std::expected<StyleSheet, StyleError>`.
The catalog is fixed after successful installation. One model is installed per
managed scene; observations can share it. A second installation is rejected before
replacing the first catalog. A failed scene setup still follows the ordinary scene
cleanup policy.

`ui::ThemeSelectionRef` is a weak UI-affine observation. It does not keep the scene
or factories alive. `snapshot()` returns an owning immutable result with catalog
metadata, status, optional `previewedId` and historical `lastCommittedId`.
The catalog owns validated labels/descriptions and IDs, without factories,
resolvers or widget/scene pointers. A copied snapshot remains readable after scene
shutdown; commands through a stale reference return a typed failure.

## Preview, commit and cancel

| Operation | Successful result |
|---|---|
| `preview(id)` | Installs the candidate as the active preview; a further preview replaces that candidate. |
| `commit()` | Promotes an active successful preview to the permanent theme and ends the preview session. |
| `cancel()` | Restores the saved theme and ends the preview session. In idle state it is an allocation-free no-op. |
| `snapshot()` | Returns independent metadata and the current authority status. |

The candidate masks the previous THEME contribution entirely: a declaration absent
from the candidate falls back through the normal cascade, not to the old theme.
APP and USER layers remain above it. Both the saved theme and candidate count
against the configured style budgets. Commit is one staged style publication;
it is not a fallible theme swap followed by a separate layer removal.

A failed preview, commit or restoration does not publish its own candidate or
partially change the model. Independent external changes made by a factory or
another reentrant hook remain in effect; failure does not roll them back.
In particular, failed cancellation does not mean the preview was removed.
Color changes can require paint; metric changes can require layout.
The model creates no polling timer and does not remount controls.

## External theme changes

A successful external theme operation retires the preview authority, even when
the resulting colors are equal. The session becomes `SUPERSEDED`; old commit or
cancel commands cannot overwrite that newer choice. Its IDs are historical and do
not identify the externally active theme. A new explicit preview starts a fresh
session from the current theme. Failed external operations preserve an active
preview. APP/USER override changes do not silently restore an older override set.

The first successful preview transfers theme authority from an existing live-theme
controller. Cancellation restores the earlier appearance; it does not resurrect
an expired controller or its old engine mode. This distinction matters when an
application mixes preview with [LiveTheme and style layers](style-layers.md).

## Errors and limits

Commands return exact `std::expected<..., ManagedError>` results. Calling a checked
command alone does not invoke the scene error collector or stop the runtime.
Returning its result from a handler uses the standard callback error path. An
application that recovers locally can report the original error using the existing
report-only diagnostic operation. Reentrant transitions are rejected; UI ownership
and closed-owner checks remain in force.

Default catalog limits are 64 choices, 128 UTF-8 bytes per ID, 512 bytes per label,
2048 bytes per description and 1 MiB of aggregate catalog text. Options expose these
positive limits. Factory captures and general allocator overhead are not a whole
process memory budget. Existing style-layer limits also apply; the model does not
silently enlarge a caller's budget.

The model performs no configuration-file I/O and invokes no persistence callback.
The [theme selector](theme-selector.md) owns its list, focus, recovery UI and
optional post-commit notification. Its open view observes external equal-theme
authority changes through existing invalidation. General theme importers and a
universal reactive system are not part of this API.
