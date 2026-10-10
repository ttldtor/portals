# Observing text properties

Include `<portals/ui.hpp>` and publish `ui::Ref<T>` through the existing
description transaction. A reference observes the widget and its scene; it
does not own either. `text()` is available for `Label`, `Button`, `CheckBox`,
public `Control` subtypes and `LineEdit`.

```cpp
using namespace portals;
ui::Ref<Label> message;
auto mounted = ui::mount(scene, ui::label("Price: ").reference(message));

if (mounted) {
  message.text() += text("{:.2f}", 123.0);
  message.text() += " USD";
}
```

`=` replaces the complete caption or draft. `+=` appends to its end, irrespective
of the editor selection. `set` and `append` provide the same operations with
explicit call-site capture and chaining. Each operation returns an owning proxy
value, so a saved chain result never borrows a temporary proxy. Its `result()`
returns `expected<void, ManagedError>` for that operation; `get()` returns an
owning `expected<std::string, ManagedError>` snapshot. Snapshots survive later
widget changes and removal.

```cpp
auto changed = message.text() = text("Price: {:.2f} USD", 123.0);

if (!changed.result()) {
  // The live scene also records the original typed mutation failure.
}
```

Proxy copy/move construction copies/transfers the observation and optional
resolver. Assigning one property to another copies the source **value** into
the receiver: `message.text() = other.text()`. Self-assignment is an equal-value
no-op. No proxy assignment silently redirects a mounted reference.

Editor proxies use `LineEditOptions::programmaticChange` (RECORD by default).
`field.text(ProgrammaticChange::RESET_BASELINE)` owns an override applying to every
mutation, including append and concatenation assignment; equal RESET clears
history without another redraw or publication. See [editor history](editor-history.md)
for Undo/Redo, modes, external revisions and read-only behavior.

## Owning read-only concatenation

```cpp
message.text() = message2.text() + message3.text();
auto saved = message2.text() + " " + text("{}", 123.0);
```

`+` reads sources into `ui::TextResult`, which owns validated `GraphemeText` or
the original typed read/resource/concatenation failure. Neither source changes,
and concatenation requests no frame. Joined bytes are resegmented across each
boundary, including combining marks and emoji ZWJ sequences. `result()` returns
an owning `expected<GraphemeText, ManagedError>` copy; this inspection can
allocate. A saved result remains usable after either source is removed.

Concatenation uses the leftmost property's width policy and **default
`TextLimits`**, rather than the source editor's draft limits. When successful
bytes are assigned/appended, the receiver validates them again under its own
width policy and limits. Different scenes on the same UI thread may therefore
measure the same result differently. No global locale or implicit cross-thread
transfer is inferred. On a failed source read, receiving assignment preserves
the source's cause and node identity; the receiving collector also records the
`text.assign.concat`/`text.append.concat` context and source acquisition origin.

## Explicit resources

```cpp
auto localized = message.text(
  [](std::string_view key) -> std::expected<std::string, TextResourceError> {
    if (key == "currency") {
      return "USD";
    }

    return std::unexpected(TextResourceError::MISSING_RESOURCE);
  });
localized += i18n("currency");
auto joined = localized + " " + i18n("currency");
```

The proxy owns the adapter; explicit reference captures remain caller-owned.
It invokes the resolver synchronously only for keys, with no borrowed widget
text across user code. Resource suffixes in `+` use the leftmost proxy/result's
captured resolver. A missing adapter yields `RESOLVER_REQUIRED`; lookup and
resolver failures retain their `TextResourceError`. UTF-8/limit failures retain
the original `TextError` and byte offset. Expression validation offsets refer
to the expression; complete-candidate validation offsets refer to the joined
candidate. Valid CR/LF/U+2028/U+2029 are rejected by LineEdit as
`INVALID_ARGUMENT`, without fabricating an invalid-Unicode error.

## Drafts, failures and lifetime

LineEdit mutations accept a local draft, move the caret/anchor to its end and
preserve its starting model revision, then apply the editor's
[confirmation mode](line-edit.md). IMMEDIATE is the
default and validates/publishes every changed candidate. ON_COMPLETION defers
publication until blur or Enter; EXPLICIT defers until Enter or programmatic
activation. All accepted changes deliver the editor's result callback.
Resolver model updates or explicit preparation do not let an older candidate
silently adopt a newer revision.

A rejected text mutation keeps the previous text, draft/selection and invalidation
state. An accepted editor candidate can subsequently fail semantic validation
or publication: it remains visible, `.result()` reports successful acceptance,
and the separate `LineEditResult` supplies VALIDATION_FAILED, CONFLICT or a
typed FAILED outcome. Earlier successful operations in a chain remain successful. Equal-value
replacement and empty append do not request redraw. Application effects made
explicitly inside resolvers or observers are not rolled back.

Construct references on the receiving UI thread. Publication occurs only after
the complete mount succeeds; failed mount leaves them empty. Copies and
destruction may move across threads after synchronized publication, but widget
operations remain UI-affine. `node()` retains the existing UI-only low-level
contract. Mutation is rejected during a frame, managed preparation/reporting,
or a nested fluent text operation, including through a second reference slot.
The caller must keep the invoking proxy alive until its call returns.

Retired nodes produce `STALE_NODE`; closed/destroyed or unbound observations
produce `CLOSED`. Operations check the immutable thread identity before mutable
scene state and return `WRONG_THREAD` on another thread. A foreign thread, an
unbound reference or a destroyed/closed scene cannot safely record into the
scene collector; the returned proxy still exposes the typed local failure.
The resolver may remove its target, close or destroy the scene; mutation
rechecks liveness afterward and guard cleanup owns only its UI-affine busy
flag. The reference does not keep the scene alive.

Expression formatting, reference construction, resolver capture and copying
owning results may allocate before a receiving operation begins. Inside the
mutation/concatenation boundary, allocation failure is typed `NO_MEMORY`, and
capacity failure is `NODE_LIMIT`. Diagnostics retain the original cause when
breadcrumb allocation fails. Checked `get()` does not report failures; fluent
mutations report to the existing bounded collector without changing runtime
stop policy. Ref-first factories, callback shorthand and a lazy scene pipeline
remain separate work.

The installed `consumer.text-property` program exercises these headers through
the relocated package:

\include{lineno} text_property.cpp
