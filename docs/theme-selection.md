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

The program is registered for
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

## Prepare expensive choices on a worker {#prepared-theme-worker}

Use `ui::preparedTheme(id, label, factory)` for parsing or other expensive preparation.
The factory accepts `std::stop_token` and returns `StyleSheet` or exactly
`expected<StyleSheet, StyleError>`. Its closure, copies, reachable data and destruction
must be worker-safe: capture immutable domain input, never a UI ref or scene.
Ordinary `ThemeChoice` nullary factories continue running synchronously on UI and
must remain short. The explicit prepared path uses the application's existing
background executor; a hosted session must supply one.

[Complete native source](examples/guide_prepared_theme.cpp):

\include{lineno} guide_prepared_theme.cpp

`preview(id)` accepts the newest intent; it does not promise installation before
return. `snapshot()` exposes `preparing`, `requestedId` and `preparationError` in
an immutable point-in-time value. While the newest request is pending, `commit()`
returns `WOULD_BLOCK` and Apply is disabled. A failed new request cannot commit an
older visible preview. Cancel revokes pending work and restores the saved preview.
A full executor queue retains the latest request and retries without another key;
a rejected completion retries the same prepared sheet without rerunning its factory.

Only the current intent, source and THEME authority lease may install. Changing
USER/APPLICATION overrides during preparation is preserved; replacing THEME
elsewhere supersedes the old request. Existing style-layer limits still apply to
the final UI installation. Engine staging, rule resolution and layout remain UI
work; preparation does not make those costs disappear. StyleError message strings
retain their existing static-storage requirement.

For a prepared `ThemeSelectorCatalog.initialId`, setup retains the intent until
an executor is available; successful preparation commits without `onCommitted`.
Its failure is a recoverable runtime preparation diagnostic, rather than a
synchronous pre-terminal setup failure. The old synchronous initial-choice path
keeps its existing setup behavior. No control is remounted and no polling timer
is introduced.
