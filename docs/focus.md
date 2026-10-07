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
and bind; `hit_test` only reads geometry. Registry storage stays bounded even if
no subsequent operation runs. An active callback retains its captures and checked
widget borrow through self-disconnect, self-removal or ancestor removal. Captures
are reclaimed on UI after their last active call and collection, or at router
destruction. During collection, capture-destructor reentry returns `would_block`;
during router destruction it returns `closed`. Capture destructors must not throw
or destroy the router themselves.

## Focus and geometry

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
Use `focused()` before entering `tree.frame` to prepare presentation state; router
operations reject frame-time access with `would_block`, so recovery never mutates
state during layout or paint.

Hit testing uses the actual `NodeGeometry.surface` and `clip`, with half-open cell
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
and `layout_tree` within `tree.frame`, renders borrowed text before that frame
ends, then sends the owning frame through FrameWriter to TestTerminal. Independent
terminal snapshots and mouse targets agree across viewport clipping, resize and
hidden-child reflow. Keyboard and programmatic actions share the same handler;
bubbling rechecks an ancestor hidden or disabled by a lower handler.

## Events and bubbling

`dispatch` accepts the owning `InputEvent` produced by `InputDecoder`. It borrows
the input only for the call. Decoder issues and probe replies are not widget
input. Plain Tab/Shift+Tab are consumed by focus traversal. Enter, Escape,
arrows, Home/End and Backspace/Delete become `ActionEvent`; modifier flags remain
available to implement selection. Press and repeat map to actions. Release and
other keys remain `KeyEvent`. Text, paste and mouse keep their event types.
Space remains text, so a LineEdit can insert it; a button can use its ordinary
activation method when it receives unmodified space.

`invoke(node, ActionEvent{Action::activate})` enters exactly the same handler path
as Enter, with no terminal-byte synthesis. The target need not be focusable.
Handlers return `EventReply::handled` to stop, or `ignored` to bubble. The router
snapshots target-to-root ancestry before delivery and checks identity, cancellation
and eligibility again before each callback. Removed or newly disabled/hidden
ancestors are skipped; surviving ancestors in the original path can still run.
A handler exception stops bubbling with `TreeError::callback_failed`; effects
already performed by that handler are not undone. The tree's error sink also sees
the failure. New registrations and nested dispatch/invoke return `would_block`.
Focus changes and token disconnect are allowed from the current handler.

`DispatchResult.status` distinguishes `no_target`, `unhandled`, and `handled`.
`target` is the initial eligible target; `handler` identifies the consumer.
Traversal can return `handled` with an empty handler. Removed nodes can still be
identified by these owning IDs. Foreign/stale explicit targets and wrong-thread
calls return a tree error before invoking user code. Ineligible explicit targets
return `no_target`.

## Limits, errors and example

`EventRouterOptions::max_registrations` must be positive. One handler per node is
allowed. Callback construction may reenter; validation repeats after final materialization.
A duplicate returns `invalid_argument`, and a full live registry returns
`node_limit`. No registration requires fresh geometry. Registry/history use
O(registration limit) storage; traversal uses bounded live-tree storage and bubble
paths use bounded tree depth. Allocation failure returns `no_memory` without
starting a handler. An allocation exception thrown by a user callback is a
`callback_failed`, following the tree's existing callback boundary.

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

  if (!action || action->action != portals::Action::activate) {
    return portals::EventReply::ignored;
  }

  widget.activate();

  return portals::EventReply::handled;
});

if (!connected) {
  return std::unexpected(connected.error());
}

auto binding = std::move(*connected);
auto result = router->invoke(button, {portals::Action::activate});
```

See [tree lifetime and frames](widget-tree.md), [decoded input](input.md),
[geometry](geometry.md), and [Scheduler ownership](scheduler.md).
