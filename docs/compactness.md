# Source compactness measurement

`portals_compactness` compares two explicitly supplied sets of complete client
source files. It is a native C++23 executable using the standard library, with
no Python, Boost, or Clang runtime dependency. It reports a vector of source
measurements, without a weighted quality score or a required reduction percentage.

## Build and run

The targets do not link Portals core. Configuring the project still performs its
normal offline dependency checks, but building these targets does not build core.
The tests use the existing vendored doctest. For Windows/MSVC, in PowerShell:

```powershell
cmake -S . -B build/compactness -G "Visual Studio 17 2022" -DPORTALS_BUILD_EXAMPLES=OFF
cmake --build build/compactness --config Release --target portals_compactness portals_compactness_tests
ctest --test-dir build/compactness -C Release -R '^portals.compactness$' --output-on-failure

& ./build/compactness/Release/portals_compactness.exe `
  --before tests/fixtures/compactness/before.cpp `
  --after tests/fixtures/compactness/after.cpp `
  --after tests/fixtures/compactness/helper.hpp --format markdown
```

For Linux/GCC:

```sh
cmake -S . -B build/compactness -DCMAKE_BUILD_TYPE=Release -DPORTALS_BUILD_EXAMPLES=OFF
cmake --build build/compactness --target portals_compactness portals_compactness_tests
ctest --test-dir build/compactness -R '^portals.compactness$' --output-on-failure
./build/compactness/portals_compactness \
  --before tests/fixtures/compactness/before.cpp \
  --after tests/fixtures/compactness/after.cpp \
  --after tests/fixtures/compactness/helper.hpp --format json
```

`--before FILE` and `--after FILE` are repeatable and both sides require at least
one file. `--format json|markdown` is optional and defaults to JSON. `--help` must
be used alone. Unknown options, missing values, repeated format options,
unsupported formats, missing/unreadable files, directories, and unsupported or
malformed lexical input return exit code 1 with a diagnostic on stderr. Reports
are emitted only after both footprints have been measured successfully.
Paths are relative to the working directory; Windows uses Unicode command-line
arguments and reports paths as UTF-8.

## Footprint and interpretation

Supply the entry point, scene, callbacks, models, adapters, and every required
local/shared helper. Nothing follows `#include`, guesses dependencies, or checks
whether the list is complete. The report records the supplied canonical paths.
Moving code from an entry point to a helper reduces the entry-only count; adding
that helper to the explicit footprint exposes its cost. The fixture command
above preserves 20 tokens across the move (9 helper tokens plus 11 entry tokens).

Each side canonicalizes paths, deduplicates aliases resolving to the same
canonical path, and sorts them before reading. Headers shared by several
examples count once in an explicitly supplied union. Hard links with distinct
canonical paths remain distinct. Do not sum per-example totals that each include
the same support file. Vendor code and Portals implementation normally belong
outside the client footprint; record reasons for exclusions alongside the report.
Changes moving client work into the public library need a separate library/API
assessment. This tool cannot establish that distinction automatically.

The only measured scope is `explicit_full_files`. The UI construction slice is
`NOT_MEASURED`; behavioral equivalence is `NOT_CHECKED`. A file containing method
and pipe variants measures both. Extracting either variant requires an explicit,
separate source artifact; this tool offers no ranges or heuristic branch removal.
There is no manifest, content hashing, or snapshot persistence in version 1:
archive inputs or identify their revision with the report for repeatability.

## Measurements

The numbers provide additional evidence. Assess compactness and readability
using these explicit criteria for each feature or before/after change:

1. Preserve required behavior, typed errors and causes, cleanup, callback and
   capture lifetimes, and ownership. Confirm them with the relevant behavioral
   tests; a shorter source does not waive these requirements.
2. Compare tokens for the complete client footprint, including helpers. Show
   absolute before/after measurements and any deliberate scope exclusions.
3. Apply the existing formatter and naming checks to both versions. Review
   meaningful whitespace, clear names, simple control flow, and visible lifecycle
   steps; passing an automatic check does not establish readability by itself.
4. Keep useful comments and error handling. Prefer an understandable configuration
   or fluent chain over a compressed expression that hides dependencies or work.
   Human review decides whether the result is clearer; there is no aggregate
   numerical definition of beauty.
5. Run cognitive complexity through existing tooling separately, for example the
   Clang-tidy check linked below. Record its configuration and limitations rather
   than relabeling lexical brace nesting or inventing a combined score.
6. Compare performance before and after features using appropriate benchmarks
   with recorded compiler, configuration, workload, and artifact. Runtime and
   allocation evidence are separate from source compactness measurements.

No minimum reduction percentage, beauty score, cognitive threshold, or universal
performance budget is defined by this source tool. Report results against the
actual project's requirements and distinguish measured facts from review judgment.

| JSON metric | Definition and aggregation |
| --- | --- |
| `files` | Number of distinct canonical file paths. |
| `source_bytes` | Sum of original input bytes, including BOM and line endings. |
| `lexical_tokens_approx` | Approximate preprocessing tokens before expansion; identifiers, numbers, literals, header names, and maximal-munch punctuators each count once. Includes directives. Comments and whitespace do not count. |
| `code_lines` | Sum of physical lines intersecting token spelling. Blank/comment-only lines do not count. Multiline raw literals and tokens spanning splices count their physical spelling lines. |
| `preprocessor_directives` | Sum of `#` or `%:` introducers at logical line start, allowing leading whitespace/comments. Conditional branches are all measured, without evaluation. |
| `preprocessor_tokens` | Sum of tokens inside directives, including the introducer and directive name. Already included in `lexical_tokens_approx`; do not add them again. |
| `comment_bytes` | Sum of comment source spelling bytes, including delimiters and internal newlines, after CRLF/CR normalization. |
| `string_literal_bytes` | Sum of string literal source spelling bytes, including encoding prefix, quotes/raw delimiter, escapes, physical splices, and contiguous user-defined suffix. Character literals and `#include` header names are excluded. This is not decoded runtime storage. |
| `max_line_bytes` | Maximum physical line length across files, in UTF-8 bytes; excludes newline and leading BOM. Tabs count as one byte, not display columns. |
| `max_brace_nesting` | Maximum lexical `{`/`}` (including digraph) nesting per file, then maximum across files. Directives, comments, and literals are excluded. Unmatched closing braces saturate at zero. |

Every metric reports `before`, `after`, signed `delta = after - before`, and
`percent = 100 * delta / before`. A zero baseline has JSON `null` or Markdown
`N/A`; it does not fabricate a percentage. Percentages use six fractional digits.
Fixed metric order, sorted paths, UTF-8 escaping, and the classic numeric locale
make JSON deterministic for the same canonical paths and bytes. Schema version
is 1 and analyzer identity is `portals-compactness-1`. Absolute canonical paths
vary across machines; reports are not portable content fingerprints.

Whitespace changes can change line/byte metrics while preserving token counts.
Comments are reported separately, without a penalty or incentive to delete them.
Brace nesting is a lexical diagnostic, not function depth, cyclomatic complexity,
cognitive complexity, or evidence of readability. Inactive preprocessor branches
can distort brace depth; syntactically unbalanced source is not rejected solely
because of its braces. Token reductions do not prove that lifetime, typed errors,
cleanup, input handling, or rendering behavior survived. Use the relevant
behavioral tests and human review before making such a claim.

## Lexical subset and failure boundaries

The scanner validates UTF-8, accepts an optional initial UTF-8 BOM, normalizes
CRLF and lone CR to LF, and removes backslash-newline splices (including C++23
horizontal whitespace between backslash and newline) with a physical source map.
It restores physical spelling inside raw literals, so a spliced sequence cannot
create a false raw terminator. Splices also join identifiers and comment
introducers. A block comment acts as whitespace; its internal physical newlines
do not terminate a directive. The newline after a line comment remains a boundary.

Supported cases include ASCII identifiers; preprocessing numbers with exponents,
digit separators, and suffixes; C++ punctuators/digraphs; line/block comments;
ordinary and raw string literals; character literals; `u8`, `u`, `U`, and `L`
prefixes; escaped quotes/backslashes; contiguous ASCII literal suffixes; and
quoted/angle header names immediately after `#include` or `#include_next`.
UTF-8 bytes are supported inside comments, strings, character literals, and header
names. Adjacent string literals remain separate tokens. Brace characters inside
literals/comments do not affect nesting.

Unicode identifiers, universal-character names outside literals, unknown source
punctuation, NUL bytes, invalid UTF-8, and the standard's special `<::` tokenization
case fail explicitly. Unterminated comments/literals, ordinary literal newlines,
empty character literals, malformed include header names, and invalid/overlong
raw delimiters also fail. Raw delimiters are limited to 16 supported ASCII bytes.
Known header contexts beyond those include directives (for example `__has_include`
or module header imports) are not modeled specially and use ordinary token rules;
their token and literal counts are approximations. Escape values, numeric syntax,
macro validity, language extensions, declarations, and C++ semantics are not
validated. Some lexically accepted inputs therefore do not compile. This is a
bounded source scanner, not a replacement C++ frontend.

The regression fixtures cover formatting-only changes, explicit moved helpers,
canonical duplicate paths, comment/literal boundaries, raw strings, escaped
quotes, UTF-8, BOM/CRLF/splices, preprocessing directives, zero baseline,
malformed input, deterministic output, and argument errors.

## Related tools and primary references

[Clang LibTooling](https://clang.llvm.org/docs/LibTooling.html) provides frontend
actions and AST-based tooling when semantic analysis is required; such integration
would need a [compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html).
[Clang-tidy cognitive complexity](https://clang.llvm.org/extra/clang-tidy/checks/readability/function-cognitive-complexity.html)
is a separate function-level diagnostic with its own algorithm and limits.
[Lizard](https://github.com/terryyin/lizard) documents NLOC, token count, and
cyclomatic complexity as distinct measures. Neither project's thresholds define
an acceptance criterion for Portals. These are references, not executable
dependencies of this tool.
