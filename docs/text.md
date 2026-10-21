# UTF-8 text and terminal width

[Русская версия](ru/text.md)

`<portals/text.hpp>` exposes `GraphemeText`, an owning UTF-8 value with immutable
[grapheme clusters and terminal-cell measurements](glossary.md#glossary-text-units).
A grapheme groups text such as a letter with its combining accent; cells measure
the screen columns occupied by that cluster. UTF-8 bytes measure its storage. The private dependency is [utf8proc v2.12.0](../vendor/utf8proc/README.md),
Unicode 18.0.0. Segmentation follows [UAX #29](https://www.unicode.org/reports/tr29/)
with stateful processing of every adjacent scalar pair. No normalization or
input repair occurs. Invalid UTF-8 returns the byte offset of the offending
sequence, including truncated tails, overlong forms and encoded surrogates.
Embedded NUL remains part of the input.

```cpp
auto text = portals::GraphemeText::from("e\xcc\x81");
if (text) {
  auto columns = text->measure(); // 1
  auto cursor = text->nextCursor(0); // byte offset 3
  auto prefix = text->clipPrefix(1); // complete cluster
}
```

Construction copies its borrowed input. `bytes()` and `graphemes()` borrow from
the resulting object until its destruction, assignment or move; copied values
own independent storage. A distinct moved-from value is empty; self-move assignment leaves the value unchanged. Concurrent reads are
safe; mutations/destruction require caller synchronization. Construction and
copy may throw `std::bad_alloc` or `std::length_error`; semantic/resource errors
use `std::expected` and never return partial text. Move construction may allocate
debug iterator bookkeeping before transferring storage; failure leaves the source
unchanged. Move assignment allocates nothing and remains `noexcept`.
Cursor and clipping operations
allocate nothing, perform no I/O and throw no exceptions. Cursor movement uses
binary search; clipping scans the fitting prefix. Cursor offsets must be whole
grapheme boundaries, including zero/end; invalid offsets return `NOT_BOUNDARY`.
Movement at the corresponding endpoint is idempotent. Negative clip budgets
normalize to zero; a two-cell cluster that cannot fit ends the prefix. These
operations do not write terminal cells; renderer continuation/overwrite rules
belong to the cell-buffer component.

Width is a deterministic supported terminal profile, **not** a font-shaping or
terminal compatibility promise. [UAX #11](https://www.unicode.org/reports/tr11/)
does not itself define terminal glyph widths. `WidthPolicy` defaults to narrow
Ambiguous characters and two-cell emoji. Wide Ambiguous mode promotes printable
one-cell scalars marked Ambiguous by utf8proc. Ordinary cluster widths sum the
positive scalar widths; combining/zero-width scalars add nothing.

Recognized emoji are keycaps (`[0-9#*]`, optional VS16, keycap mark), regional
indicator clusters (including an unmatched indicator), and clusters with an
Extended_Pictographic base and either native scalar width two, VS16, ZWJ, modifier
or tag sequence. VS15 suppresses that pictographic promotion. This is a small
profile, not a complete RGI emoji validator. The emoji one/two-cell choice applies
to single recognized emoji as well as compound sequences. Additional combining
marks stay with their logical cluster. The implementation uses upstream
properties, not a parallel Unicode table.

A zero-cell or ordinary greater-than-two-cell cluster is `unsupported`, measured
as one cell. Controls (including CRLF, tab, ESC, NUL and C1), unassigned scalars,
line/paragraph separators and unsupported formatting clusters are also
`unsupported`. A renderer **must display ASCII `?` and must never emit their
original bytes**. Logical text and boundaries are preserved, allowing the full
official grapheme corpus to test segmentation independently of printability.
There is no multiline/tab expansion in this component.

Default caps are 1 MiB input, 64 KiB per cluster, 1,048,576 clusters and 2,097,152
cells. Callers can change the caps; input length is also bounded by `PTRDIFF_MAX`
and accumulated cells by `INT32_MAX`, matching cell geometry. Empty text is valid
with zero caps. No grapheme is silently truncated when a cap is exceeded. Cell
addition is checked before arithmetic. Resource errors identify the rejected
cluster's start; input-length and invalid-policy errors use offset zero.

The full captured `WidthPolicy`, including caller capability `revision`, belongs
in measurement/layout/cell-buffer cache keys. Changing it requires constructing
new measurements and invalidating affected geometry and paint. The object offers
no mutable policy setter and triggers no UI invalidation itself.

Headless tests cover semantic widths, UTF-8 failures, controls, ownership, limits,
cursor/clip boundaries and the complete official Unicode 18 GraphemeBreakTest
corpus, with no filtering. Vendored file and fixture hashes are checked without
network access. Width fixtures test this policy rather than any physical terminal.

## Secret bytes with SecureString {#text-secure-string}

[`SecureString` and `SecretView`](../include/portals/secure_string.hpp) provide
move-only byte storage and synchronous plaintext access through `<portals/secure_string.hpp>`.
Link the [installed package](package.md) target `portals::core`. `SecureString`
preserves application bytes, including embedded NUL, without Unicode validation,
normalization, SSO or an implicit conversion to ordinary text.

`create`, `assign` and `append` copy a borrowed `std::string_view` during the call;
keep its input buffer valid until that call returns. Use `std::string_view(data, size)`
for bytes containing NUL. Caller-owned input and copies made by external APIs remain
outside this storage guarantee. `maxBytes` defaults to 4096 bytes and accepts
0 through 1048576; zero permits only empty values. When new storage is needed, a
mutation prepares an allocation for its final logical length, rounded to payload
pages for native storage.

This complete example uses an ASCII sample value, reads only its length and closes
the storage explicitly. It produces no output and opens no terminal:

```cpp
#include <portals/secure_string.hpp>

#include <cstddef>

int main() {
  auto secret = portals::SecureString::create("sample");

  if (!secret || !secret->append("-bytes")) {
    return 1;
  }

  std::size_t byteCount{};
  auto accessed = secret->withPlaintext([&](portals::SecretView view) {
    byteCount = view.size();
  });

  if (!accessed || byteCount != 12 || !secret->clear()) {
    return 2;
  }

  return secret->close() ? 0 : 3;
}
```

By default, Windows and Linux use dedicated pages with an always-writable payload
and two inaccessible exterior guard pages. Without requested Windows idle encryption,
the live payload contains plaintext between callbacks. Wiping covers its entire
accessible capacity, including padding and unused tails. The independently requested
options are mandatory: unsupported guarantees return `UNSUPPORTED`; native setup
failure returns a typed error without silently weakening the policy.

| Guarantee | Default | Windows | Linux | WASM / other unsupported platforms |
| --- | --- | --- | --- | --- |
| Capacity wiping | Required | Applied | Applied | Applied with explicit `WIPE_ONLY` |
| Exterior guard pages | Required by `NATIVE_GUARDED` | Applied | Applied | Unsupported |
| `lockPages` | `false` | Native page lock | Native page lock | Unsupported |
| `excludeFromDumps` | `false` | Unsupported | Linux core-dump exclusion | Unsupported |
| `encryptWhileIdle` | `false` | `CryptProtectMemory`, `SAME_PROCESS` | Unsupported | Unsupported |

On WASM, default creation returns `UNSUPPORTED`, including for empty input. To accept
wiping alone explicitly, pass `SecureStringOptions` with
`storage = SecureStringStorage::WIPE_ONLY` to `create`; this policy uses heap or
linear-memory storage and rejects lock, dump and encryption requests on every platform.
`capabilities()` reports guarantees actually applied. These mechanisms do not protect
against a debugger or ArtMoney with process access, hostile code in the process,
the terminal host, keyloggers, live plaintext access or external copies. Length remains visible.

`withPlaintext` owns a copy or move of its callable and holds storage alive through
the callback. A `SecretView` exposes explicit `data()` and `size()`; it expires when
the callback returns. Do not retain its pointer or a view made from it. C++ cannot
prevent a callback from saving that pointer or copying the bytes; those escaped
values remain the caller's responsibility. Callbacks return
`void` or `std::expected<void, E>` with an owning, nothrow-movable error; direct borrowed
errors and coroutine/task results are rejected. Custom error wrappers and referenced
callable captures retain their caller-managed lifetime obligations.

Moving or destroying the outside owner during the callback preserves the active view.
The moved handle retains the same busy state: nested access and checked mutations,
including empty append, clear and close, fail with `BUSY`. Operations, moves and
destruction require external synchronization; concurrent mutable use is unsupported.

Allocation/capability preparation failure preserves the old value. Ambiguous crypto
failure wipes the affected payload and makes it unavailable; a replacement that has
retired old storage never restores old bytes. Library diagnostics contain only operation,
category and native code. Callback errors or exceptions remain separate from cleanup
failures; opaque `callbackError` is never logged by the library. `cleanup` retains its
first four errors in order and counts further errors in `omittedCleanupCount`.

`clear()` leaves a valid empty value under the same policy. `close()` wipes and releases;
it is idempotent after success. A release failure leaves the value unavailable and keeps
the zeroed allocation metadata for a checked `close()` retry. Destruction makes at most
two release attempts without throwing; persistent OS failure can leak zeroed storage.
