# Text formatting and table columns {#compact_examples}

[Русская версия](ru/compact-examples.md)

Start with the [runnable examples](examples.md), then read
[declarative composition](declarative-ui.md) for ownership and mount errors.
This page covers checked text formatting, resource values and table columns.

## Format and validate text

Include `<portals/text_format.hpp>`. `formatText("Price {:.2f}", value)` checks
the format specification at compile time and returns
`expected<GraphemeText, TextError>`. It validates the resulting UTF-8 and preserves
the original error category and byte offset. Use
`formatText(policy, "Price {:.2f}", value)` for an explicit display policy.
Allocation and custom standard-formatter exceptions can still propagate.

For a table callback, `cellFormat<double>("{:.2f}", policy)` owns its pattern
and policy and accepts a borrowed `const double &`. It returns exactly owning
`GraphemeText`, following the existing callback contract. Direct invocation can
throw `bad_expected_access<TextError>` for invalid formatted bytes; table
preparation translates thrown exceptions to its existing `CALLBACK_FAILED`.
Use checked `formatText` when the application needs the individual text error.
This convenience does not introduce a second callback error protocol.

## Define fixed table schemas

This fragment assumes an `Ohlc` record with `double open` and `double close`
fields, and a `portals::WidthPolicy policy` variable. Include
`<portals/table_column.hpp>` and `<portals/text_format.hpp>`.

```cpp
const auto price = portals::cellFormat<double>("{:.2f}", policy);

auto columns = portals::tableColumns(policy,
  portals::tableColumn("Open", &Ohlc::open, price, 9),
  portals::tableColumn("Close", &Ohlc::close, price, 9));

if (!columns) {
  // Report columns.error(): the original header TextError.
}
```

The record type is inferred from the member pointer. Column descriptions (`TableColumnSpec<T>`) own captions and
callbacks; assembly validates captions and assigns IDs 1..N with initial generation
in argument order, independently for each table. Assemble a fixed schema once.
Reordering the resulting columns retains their IDs. Rebuilding column descriptions in a new
order creates a different positional schema; dynamic replacement uses explicit
`ColumnId` through the original factories. No global counter or caption-derived
identity is introduced. The existing table validates widths during preparation. The formatter
must use the same width policy as the header and table. No domain row is copied.
`columnOptions(id, caption, width, policy)` returns checked metadata for aggregate
fields or explicit lambda projections. Display captions may be runtime strings:
they are not used as identities or assumed to be translation keys.

## Owning text and resource arguments

Include `<portals/text_value.hpp>` for `text("literal")`,
`text("{}", 123.0)` and `i18n("currency")`. Each expression owns its bytes or
resource key. Formatting with arguments uses a compile-time checked standard
format string. Without arguments, `text("{}")` is literal text, including braces.
Literal/key construction and key inspection are `constexpr`; formatting and
grapheme validation remain runtime operations in C++23. This does not guarantee
that every owning string can be stored in a persistent constexpr object.
Formatting runs immediately; UTF-8 validation and width measurement run during
`resolve`, using the receiving context's explicit `WidthPolicy` and `TextLimits`.

```cpp
using namespace portals;
const auto amount = text("{:.2f}", 123.0);
const auto unit = i18n("currency");
const auto translated = unit.resolve(
  WidthPolicy{}, [](std::string_view key) -> std::expected<std::string, TextResourceError> {
    if (key == "currency") {
      return "USD";
    }

    return std::unexpected(TextResourceError::MISSING_RESOURCE);
  });
const auto formatted = amount.resolve(WidthPolicy{});
```

Resolution returns `expected<GraphemeText, TextValueError>`. The error variant
retains the original `TextError`, or identifies a required resolver, missing
resource or failed resolver through `TextResourceError`. A literal never calls
the resolver. Resource values resolve each time they are used; Portals does not
cache translations or keep a global locale. The callable is synchronous and
must return an owning `expected<std::string, TextResourceError>`.

Allocation and capacity exceptions propagate. Other resolver exceptions become
`RESOLVER_FAILED`; their exception payload is not retained. Formatter exceptions
propagate from `text` before resolution. `resourceKey()` borrows the stored key
and remains valid only while its expression is alive and unmodified.

Use these values with [fluent text properties](text-property.md) through
`message.text() = text("{}", 123.0)` or `message.text() += i18n("currency")`.
Resource expressions require an explicit resolver on the proxy. Automatic
language switching and resource-file loading remain separate work.

## Optional aggregate fields

Link `portals::aggregate` and include `<portals/aggregate_columns.hpp>` only where
aggregate reflection is useful. This opt-in target links the existing core and
supplies the standalone pinned Boost.PFR headers and C++23 configuration.
The ordinary `portals::core` target does not add PFR to its include search path
or compile definitions. Building and consuming the installed adapter require no
network, Boost superproject, or Git submodule.

`aggregateColumns<Record>(std::array{ColumnCaption{"Bid", 9}, ColumnCaption{"Ask", 9}},
formatter)` validates captions and assigns fixed-schema identities automatically.
The overload taking `TableColumnOptions` retains explicit identities for dynamic schemas.
Both forms project every field in aggregate
declaration order, with an independent owning formatter in each existing column.
Metadata count must equal field count, and the formatter must accept every const
field type and return exactly `GraphemeText`. `aggregateColumn<I, Record>` selects
a single field. Explicit projections remain the path for nested records,
nonaggregates, and different formatting of different fields.

PFR's portable SimpleAggregate domain excludes base classes, reference/const
members, and C array members. Unsupported shapes are not promised to produce
a friendly constraint diagnostic. Field positions are not stable schema IDs:
keep explicit column identities stable when record declarations change.
Reflection does not provide captions, units, validation rules, edit commands,
resources, or i18n. It does not copy datasets or change model ownership.

The target sets `BOOST_PFR_USE_CPP26=0` to select the C++23 backend consistently.
Keep PFR version and configuration consistent across translation units; do not
mix another Boost.PFR version in the same program without checking compatibility.
The [dependency notice](../vendor/pfr/README.md) records provenance and hashes.


## Match key events

Include `<portals/input_match.hpp>` for C++23 constexpr predicates. This
fragment assumes `InputEvent event` in a handler returning `bool`:

```cpp
if (keyPressed(event, InputKey::F2)) {
  // Switch theme, preserving its typed failure handling.
}

if (!keyPressed(event, {InputKey::LEFT, InputKey::RIGHT})) {
  return false;
}

```

Matching accepts PRESS and REPEAT, excludes RELEASE and non-key input, and does
not consume events or track physical held-key state. The single-key and list
overloads ignore modifiers, using modifier-independent matching. For an exact
shortcut use `keyPressed(event, InputKey::F2, {.control = true})`; all modifier
fields must match, including false fields. CHARACTER matches identity only:
inspect `KeyEvent::scalar` or `TextInputEvent` for character content. The key list
allocates nothing and is searched linearly. Routing and focus still belong to the
existing router; this convenience adds no key-binding subsystem.

## Scene examples

The [declarative guide](declarative-ui.md) contains complete applications with
event handlers and shared state. For virtual tables and charts, see the
[dashboard](../examples/dashboard/dashboard.hpp).
