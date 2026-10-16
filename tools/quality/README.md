# Additional quality evidence

[Developer tool catalog](../../docs/developer-tools.md) · [Command reference](../../docs/developer-tools-measure.md)

These CMake 3.24+ drivers have no Python runtime requirement:

- `readability.cmake`: explicit translation units, optional explicit headers, and an existing compilation
  database, installed Clang-tidy cognitive complexity at reporting Threshold 0,
  optional existing readability checks, preserved raw logs/version/SHA receipts.
- `performance.cmake`: `MODE=record` archives existing managed/compact/MVP JSON
  with explicit provenance; `MODE=compare` checks compatibility and reports
  available p50/p95/allocation/node/frame observations without a causal verdict.

See [the full workflow and limitations](../../docs/quality.md). Existing formatter,
naming, behavioral tests, and human review remain independent evidence. No
overall score, invented complexity, performance framework, or Git operation is
introduced. Every receipt directory is immutable by these drivers.

`change_impact.py` is a separate Python 3.10+ standard-library authoring tool.
It reads Ninja dependency/compilation captures without building and reports
observed translation units, CMake targets and public-header fan-in. Its lexical
include graph is separate from compiler-observed transitive dependencies.
Run `python tools/quality/change_impact.py --help`; see the
[EN/RU command reference](../../docs/developer-tools-measure.md#tool-change-impact).
`python tools/quality/test_change_impact.py` runs its disposable parser/CLI
fixtures. These tools add no SDK runtime dependency or CI selection policy.
