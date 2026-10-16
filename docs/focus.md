# Focus, input routing and semantic actions

`EventRouter` is a synchronous UI component over `WidgetTree`. It owns focus and a
bounded registry, and uses the existing tree's checked `edit` borrow for each typed
handler. It does not own the tree, create a second scheduler, queue events, or write
terminal bytes. Include `<portals/action.hpp>` and link `portals::core`.

## Ownership and lifetime

Create and use the router on the tree's UI thread. Keep Scheduler, then WidgetTree,
then EventRouter alive in that order; destroy in reverse order. Destroying the
router inside an active handler/collection or on another thread terminates.
Handlers receive `T&` and `const RoutedEvent&` borrowed only until they return.
Copy event data if it must survive the call. A returned `DispatchResult` owns its
`NodeId` values; those identities remain useful for diagnostics after removal but
do not keep nodes alive.

Keep the move-only `ActionBinding` for as long as delivery is wanted. Dropping or
explicitly disconnecting it prevents future admissions. Tokens are observers and
may outlive the router. Tokens do not extend widget or tree lifetime. One token
object must not be accessed concurrently with its own move/destruction.

`ActionBinding::disconnect` may run on a worker: unlike a Scheduler subscription,
it only stores `false` in a separate atomic cancellation object. It never acquires
an owner of the callback or its captures. The sequentially consistent load of
`true` when resolving a handler is the admission point; the store of `false` is
the disconnect point. A callback admitted before disconnect may start or finish
after disconnect returns. A later admission is rejected. Disconnect is not a
join and cannot interrupt user code.

The registry and a currently executing handler hold callback owners only on UI.
Removed/disconnected records are collected by focus operations, dispatch, invoke,
and bind; `hitTest` only reads geometry. Registry storage stays bounded even if
no subsequent operation runs. An active callback retains its captures and checked
widget borrow through self-disconnect, self-removal or ancestor removal. Captures
are reclaimed on UI after their last active call and collection, or at router
destruction. During collection, capture-destructor reentry returns `WOULD_BLOCK`;
during router destruction it returns `CLOSED`. Capture destructors must not throw
or destroy the router themselves.

## Focus and geometry

The additive `bind(node, inputCallback, focusCallback, focusable)` overload owns
both callbacks under one ordinary ActionBinding. The optional focus callback
takes `(WidgetType&, bool focused)` and returns `expected<void, TreeError>`.
The input-only overload keeps its existing behavior and RoutedEvent is unchanged.
Transitions deliver direct loss then gain synchronously before further routed
input; they do not bubble. Live hidden/disabled previous targets receive loss;
removed or disconnected targets are skipped, and gain is rechecked after loss.
Gain is attempted even after a loss error; the first loss/gain error is returned.
Nested focus changes/input delivery return WOULD_BLOCK; inspection of a still
eligible current focus remains available. Callback errors are typed and do not
undo already published focus or model effects. These scoped callbacks implement
LineEdit completion-on-blur through the existing router.

Focusable registrations participate in structural preorder: parent before child,
children in insertion order. Registration order and slot reuse do not change it.
Tab advances and Shift+Tab reverses with wrap. With no current eligible focus,
forward traversal chooses the first eligible node and reverse chooses the last.
Hidden or disabled ancestors exclude the whole subtree.

Focus recovery is lazy, at `focused()` or before keyboard/text/paste delivery.
If current focus was removed, disconnected, hidden or disabled, choose the most
recently focused eligible node, otherwise the first preorder node, otherwise an
empty `Node<Widget>`. History contains each registered node at most once and is
bounded by the registration limit. Merely showing a previously hidden node does
not steal valid current focus. Focus changes request paint invalidation, not layout.
`focused()` and `traverse()` recheck their selected registration after callbacks.
If it has been removed, disconnected, hidden or disabled, this call returns an
empty observer; a later call can recover again. Keyboard/text/paste delivery then
returns `NO_TARGET`. A mouse event whose original hit or ancestor was retired by
focus callbacks also returns `NO_TARGET`, without redirecting the event to another
focus or hit. Callback errors still propagate before any routed input.
Use `focused()` before entering `tree.frame` to prepare presentation state; router
operations reject frame-time access with `WOULD_BLOCK`, so recovery never mutates
state during layout or paint.

Each focus-order walk builds a temporary sorted index of full focusable registration
identities (session, slot, generation). The tree still determines visitation
order; the index only replaces a linear handler lookup at each visited node.
Cancellation is checked when a registration is used, rather than cached when
the index is built. Callback ownership remains in the router on the UI thread.

For R registrations and V visited nodes, index construction and registration
lookup take O((R + V) log(R + 2)) work and O(R) additional temporary storage.
This bound excludes tree snapshots, collection and focus-history recovery.
In particular, collection still scans registrations and looks up history entries
linearly; this is not an overall complexity guarantee for `focused()` or Tab.
See the [managed performance measurements](managed-performance.md) for the
measured workload and remaining costs.

Hit testing uses the actual `NodeGeometry.surface` and `CLIP`, with half-open cell
coordinates. Every ancestor must have fresh geometry and be visible/enabled.
Reverse paint order wins: a later sibling's subtree is above an earlier one, and a
child is above its parent. A hit can be an unregistered child; left-button press
focuses its nearest focusable ancestor and then bubbles the unchanged mouse event.
There is no pointer capture, implicit mouse activation or synthetic click.

Layout invalidation makes mouse geometry unusable until the actual layout owner
arranges it again. A miss returns an empty node. Stale geometry does not exclude
an otherwise live, enabled, visible node from keyboard traversal or delivery.
The focused routing tests arrange real tree nodes explicitly. The separate
`tests/focus_layout.cpp` integration uses actual [Row/Column layouts](layout.md)
and `layoutTree` within `tree.frame`, renders borrowed text before that frame
ends, then sends the owning frame through FrameWriter to TestTerminal. Independent
terminal snapshots and mouse targets agree across viewport clipping, resize and
hidden-child reflow. Keyboard and programmatic actions share the same handler;
bubbling rechecks an ancestor hidden or disabled by a lower handler.

## Nested focus scopes

Declare a subtree with `EventRouter::focusScope(root, defaultTarget)` and retain
its `ActionBinding`. Scope and action records share `max_registrations`; one of
each can be registered on a node. A scope owns exact session/slot/generation
observers and an independent cancellation flag. Disconnect can run on a worker;
the next UI collection reclaims its record. It never owns a widget or callback.

`enterScope(root)` selects the eligible last focused descendant, then its default,
then the first eligible preorder member. A removed, hidden, disabled or
disconnected target is skipped. An empty eligible subtree clears focus. The
configured default must initially belong to the subtree; it need not be focusable
or eligible until entry. `focusDefault(node)` replaces the nearest enclosing
scope's default. With several declarations, the last preorder declaration wins.
Scope registration itself leaves current focus unchanged and remembers an existing
focused descendant. Explicit `setFocus` can select a control outside all scopes.

Every transition remembers its exact target in all containing scopes. Tab and
Shift+Tab visit the complete global preorder and reverse preorder, with wrap;
scope boundaries never skip a control. Initial global focus keeps the existing
first-eligible policy. Only explicit scope entry and recovery use last/default
selection. A remembered or default target is selected exactly, including when it
lies in a nested scope. Explicit-entry fallback recursively enters nested scopes.

When focus becomes ineligible, recovery tries its remembered containing scopes
from nearest to outermost, then the existing global history/preorder policy.
Scope membership uses allocation-free parent inspection, independent of geometry.
Scope selection scans eligible order and scope ancestry; this adds work for scope
entry/recovery and is not covered by the registration-index complexity above.
No polling, timers or idle redraw are added. Reentering the current target does
not request paint. Allocation failure before focus publication leaves the previous
focus unchanged. Loss/gain errors retain the published focus as described above.

For declarative clients, ManagedScene retains scope tokens automatically:

```cpp
ui::Ref<Column> form;
ui::Ref<Button> apply;
auto recipe = ui::column(form,
    ui::button("Back", [] {}),
    ui::button(apply, "Apply", [] {}).focusDefault()).focusScope();
auto mounted = ui::mount(scene, std::move(recipe));
if (!mounted) {
  return std::unexpected(mounted.error());
}
auto focused = form.focus();
```

Here `scene` is an existing UI-thread `ManagedScene`. Check `focused` as an
ordinary `expected<void, ManagedError>`. `Ref::focus()` enters scope refs and
focuses ordinary registered controls through the existing blur/validation path.
A void action may call `(void)form.focus()` and let its owning scene collect an
error once. Directly returning `form.focus()` is not a supported compact action
result. An unpublished/unowned Ref has no scene collector: its checked `CLOSED`
result must be handled by the caller; no unrelated ambient scene is inferred.
References retain weak scene lifetime: unpublished/closed refs return `CLOSED`,
removed targets return `STALE_NODE`, and foreign-thread calls return `WRONG_THREAD`.
Failed mounts publish no refs and retire scope tokens with their subtree. Scope
declarations are processed before child defaults. A default without an enclosing
scope fails the mount with `INVALID_ARGUMENT`.

The complete [installed consumer](../tests/package_consumer/focus_scopes.cpp)
checks default entry, remembered return, Tab exit, stale identity and closed refs.
Scopes do not implement modal capture or automatic modal return.

## Saving and returning focus

`enterFocusScope(root)` saves the exact current focus and its enclosing scopes,
then enters a declared eligible scope using last/default/first selection.
`leaveFocusScope(root)` restores that saved focus. These operations use a focus-return
stack owned by the existing router. They do not install input shielding or a Tab
trap: global Tab order, explicit programmatic focus outside and mouse targeting
remain available. Input-modal interaction is not provided by this API.

Scopes may nest with different roots. Entering a root already in the stack, or
leaving a root other than the top, returns `INVALID_ARGUMENT`. An eligible scope
without focusable members enters with empty focus and still retains its return
record. The existing scope registration token controls registration lifetime;
clients do not store another token. Stack depth is bounded by `max_registrations`;
exhaustion returns `NODE_LIMIT`. Saved ancestry adds storage proportional to the
sum of saved scope depths. No timer or idle frame request is added.

Return first checks the saved session/slot/generation and eligibility. If that
target is gone, it tries the saved enclosing scopes from nearest to outer, then
eligible global history and the first global preorder node. Reusing a slot never
substitutes a new generation. Hidden, disabled, removed or disconnected activation
roots end lazily at `focused()` or input dispatch. Ended outer entries wait behind
an active inner entry; consecutive ended entries unwind together on return.
An automatic return consumes that Tab/Shift+Tab navigation step, so its callback
cycle is not immediately followed by another traversal. The following Tab resumes normal global traversal. A mouse event that performs this return also skips its automatic focus-on-press step. Showing an ended root
does not recreate its activation. Disconnecting and registering the same root
again also does not revive its old record.

All allocation and validation before publication preserve focus and the stack on
failure. Once published, an entry callback error retains the new focus and saved
record; the caller can still leave. A published return consumes its record even
if its loss/gain callback reports an error. Callback self-removal returns an empty
checked observer; a later focus operation may recover. Nested enter/leave from
a focus callback returns `WOULD_BLOCK`. Ordinary action callbacks may enter/leave.
The existing LineEdit IMMEDIATE, ON_COMPLETION and EXPLICIT blur/validation rules
continue to apply; rejected validation does not discard a surviving dirty draft.

With an existing UI-owned `ManagedScene scene`, declare the scope and capture its
observing Ref in ordinary void callbacks:

```cpp
ui::Ref<Column> panel;
ui::Ref<Button> accept;
auto recipe = ui::column(
    ui::column(panel,
        ui::button(accept, "Accept", [] {}).focusDefault()).focusScope(),
    ui::button("Open", [panel] { (void)panel.enterFocusScope(); }),
    ui::button("Back", [panel] { (void)panel.leaveFocusScope(); }));
auto mounted = ui::mount(scene, std::move(recipe));
if (!mounted) {
  return std::unexpected(mounted.error());
}
```

`ManagedScene::enterFocusScope(node)` and `leaveFocusScope(node)` expose the same
operations. `Ref` methods return `expected<void, ManagedError>` and report owned
failures once to the scene collector. An already reported editor blur failure retains
its original editor identity and reporting receipt, including across scenes in the same
Application. Automatic return during managed prepare and input traversal preserves
the same single managed diagnostic for one failed blur. The destination scene does
not report it again. Independent result/gain failures remain separate diagnostics;
the existing ApplicationError runner envelope keeps its own policy. Check that result when explicit error handling
is needed. An unpublished/destroyed owner yields `CLOSED`; a removed managed Ref
yields `STALE_NODE`. Its activation recovers through the router, without dereferencing
the stale Ref. The complete installed-consumer program is
`tests/package_consumer/focus_return.cpp`; it exercises entry, return and external hide
after SDK installation and relocation.

## Events and bubbling

`dispatch` accepts the owning `InputEvent` produced by `InputDecoder`. It borrows
the input only for the call. Decoder issues and probe replies are not widget
input. Plain Tab/Shift+Tab are consumed by focus traversal. Enter, Escape,
arrows, Home/End and Backspace/Delete become `ActionEvent`; modifier flags remain
available to implement selection. Press and repeat map to actions. Release and
other keys remain `KeyEvent`. Text, paste and mouse keep their event types.
Space remains text, so a LineEdit can insert it; a button can use its ordinary
activation method when it receives unmodified space.

`invoke(node, ActionEvent{Action::ACTIVATE})` enters exactly the same handler path
as Enter, with no terminal-byte synthesis. The target need not be focusable.
Handlers return `EventReply::HANDLED` to stop, or `IGNORED` to bubble. The router
snapshots target-to-root ancestry before delivery and checks identity, cancellation
and eligibility again before each callback. Removed or newly disabled/hidden
ancestors are skipped; surviving ancestors in the original path can still run.
A handler exception stops bubbling with `TreeError::CALLBACK_FAILED`; effects
already performed by that handler are not undone. The tree's error sink also sees
the failure. New registrations and nested dispatch/invoke return `WOULD_BLOCK`.
Focus changes and token disconnect are allowed from the current handler.

`DispatchResult.status` distinguishes `NO_TARGET`, `UNHANDLED`, and `HANDLED`.
`target` is the initial eligible target; `handler` identifies the consumer.
Traversal can return `HANDLED` with an empty handler. Removed nodes can still be
identified by these owning IDs. Foreign/stale explicit targets and wrong-thread
calls return a tree error before invoking user code. Ineligible explicit targets
return `NO_TARGET`.

## Limits, errors and example

`EventRouterOptions::max_registrations` must be positive. One handler per node is
allowed. Callback construction may reenter; validation repeats after final materialization.
A duplicate returns `INVALID_ARGUMENT`, and a full live registry returns
`NODE_LIMIT`. No registration requires fresh geometry. Registry/history use
O(registration limit) storage; traversal uses bounded live-tree storage and bubble
paths use bounded tree depth. Allocation failure returns `NO_MEMORY` without
starting a handler. An allocation exception thrown by a user callback is a
`CALLBACK_FAILED`, following the tree's existing callback boundary.

The snippet assumes a live `Node<MyButton> button` and a UI-owned `WidgetTree tree`.
`MyButton::activate` is application code; the tree must be explicitly invalidated
when that action changes presentation.

```cpp
auto created = portals::EventRouter::create(tree);

if (!created) {
  return std::unexpected(created.error());
}

auto router = std::move(*created);
auto connected = router->bind(button, [](MyButton &widget, const portals::RoutedEvent &event) {
  const auto *action = std::get_if<portals::ActionEvent>(&event);

  if (!action || action->action != portals::Action::ACTIVATE) {
    return portals::EventReply::IGNORED;
  }

  widget.activate();

  return portals::EventReply::HANDLED;
});

if (!connected) {
  return std::unexpected(connected.error());
}

auto binding = std::move(*connected);
auto result = router->invoke(button, {portals::Action::ACTIVATE});
```

See [tree lifetime and frames](widget-tree.md), [decoded input](input.md),
[geometry](geometry.md), and [Scheduler ownership](scheduler.md).
