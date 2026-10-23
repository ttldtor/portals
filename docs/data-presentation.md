# Present data loading states {#data-presentation}

[Русская версия](ru/data-presentation.md)

Wrap content in `ui::dataStateView` to reserve a status strip for loading, empty,
failed, offline and refreshing states. The strip contains an animated indicator
only while work is pending. Ordinary content remains an ordinary retained subtree;
the wrapper does not virtualize it or start data requests.

## Run an explicit presentation

This complete application changes presentation with buttons. It starts ready,
keeps its label visible in every state and exits with **Exit** or F10. A button's
checked `display` result reaches the standard `runApp` diagnostics. The scene owns
the controls; `presentation` is an observing reference.

\include guide_data_state.cpp

To build this guide in the Portals source tree, enable `PORTALS_BUILD_TESTS`, then
build and run `portals_docs_data_state`. For your own application, link
`portals::core` as described in [using the installed package](package.md).

`ui::dataStateView(DataPresentation{...}, content)` owns its presentation value.
Only this mode accepts `ui::Ref<DataStateView>::display` or the explicit checked
`setDataStateView(tree, node, value)` operation. Use `DataContentPolicy::REPLACE`
in `DataStateConfig` when independently owned work permits hiding the body outside
`READY` and `REFRESHING`. The default `KEEP_BODY` keeps it live.

## Observe scene state

Pass a same-scene `ManagedState<DataPresentation>` or a read-only
`ManagedSource<DataPresentation>` instead of the initial value. For example, inside
a scene factory with a `ManagedScene &owner`:

```cpp
return owner.state(DataPresentation{DataPhase::LOADING})
  .transform([](auto state) {
    return ui::dataStateView(state, ui::label("Results"));
  });
```

The producer changes its state through its existing checked `set` operation.
`display` rejects both bound forms with `TreeError::INVALID_ARGUMENT`; binding a
writable state does not give the view a writable alias.

For explicit and bound values, `generation` must be nonzero. A value older than
the last accepted generation is ignored after thread, lifetime and frame checks.
An equal generation may change phase, including pending to terminal; an equal
value is a no-op. Advance the producer's generation when cancelling or superseding
work: presentation cannot distinguish competing completions with the same number.
The selected producer or binding remains fixed for the mounted view.

## Follow a virtual view

Use the overload with only a managed `VirtualList` or `VirtualTable` recipe to
follow its prepared viewport range. Given the `model` and formatter from
[the virtual list guide](virtual-list.md), replace its returned recipe with:

```cpp
return ui::dataStateView(
  ui::virtualList(model, [](const ModelRow<std::string> &row) {
    return displayText(row.value);
  }));
```

The wrapper reads owning status facts from the body's same successful preparation.
It does not pin or query the model again, maintain a second cache or install a query
subsystem. The body remains live in all phases so its existing provider callback
can receive and complete work. `REPLACE` and `display` are rejected in this mode.
The wrapper's fixed presentation generation of 1 is separate from model request
generations and revisions.

| Prepared range | Presentation |
| --- | --- |
| No preparation yet, pending rows, or an active request without ready rows | `LOADING` |
| Ready rows with an active request | `REFRESHING` |
| Failed range without an active request | `FAILED` |
| Successful zero logical row count | `EMPTY` |
| Ready nonempty rows without an active request | `READY` |

`OFFLINE` is explicit domain information; the wrapper never infers it from an
error message. Refreshing preserves only content the existing provider/view permits
to remain ready; it does not invent stale data. An explicit retry of a failed base
range retains its current request until completion or ordinary cancellation, even
when indicator frames prepare the view again.

## Labels, retry and failures

`DataStateConfig::labels` owns `TextValue` messages and the optional retry caption.
Literal UTF-8 and resource keys use the receiving scene's width policy. Supply
`resolver` for resource keys; resolution and text validation happen at mount, with
no automatic locale subscription. To change locale, construct a new composition
with the desired labels and resolver.

The standard theme applies to the status composition. `messageStyle` and
`retryStyle` supply manual styles beneath theme resolution. The semantic classes
are `data-status`, `data-spinner`, `data-message` and `data-retry`; see
[style layers](style-layers.md) for precedence.

`retry` is an optional owning callback returning `std::expected<void, TreeError>`.
The retry button appears for `FAILED` and `OFFLINE`; only explicit activation calls
it. For automatic views, use the body's existing `retryVirtualList` or
`retryVirtualTable` operation in that callback. Pending work suppresses repeated
activation. No request starts just because the strip appears, and no focus is
stolen when an error appears.

A domain `DataPhase::FAILED` is separate from a technical failure. Invalid options,
allocation failures, formatter/provider callbacks and text-resource errors retain
their original typed causes through existing managed diagnostics. A technical
failure does not become an offline or failed domain state. Initial mount is checked
and rolls back on refusal. In own-value mode, a refused `display` or
`setDataStateView` update publishes no partial status or text. This guarantee covers
presentation changes; removals by application callbacks and model updates retain
their own effects and existing typed error and stop policy.

The strip reserves `statusRows` terminal rows (default 1), including while ready.
It uses one scoped one-shot timer at `spinnerInterval` (default 100 ms), rearmed
only for visible, enabled pending content with a nonempty clip. Hidden, removed or
closed views stop animation; ready and static states do not poll. Set `showStatus`
to false to omit the strip, labels, retry button and timer. In that configuration,
`retry` must be empty; a configured retry callback makes mount return
`TreeError::INVALID_ARGUMENT`.

See the [public data presentation declarations](../include/portals/data_presentation.hpp)
for settings and the [declarative API](declarative-ui.md) for scene lifetime and
error handling.
