# Quality evidence for feature changes

Assess compactness and readability alongside behavioral and performance evidence.
The tools below produce additional observations; they do not decide whether a
feature is correct, beautiful, or accepted. Required behavior, typed error causes,
cleanup, capture lifetimes, and ownership remain mandatory. Use the existing
formatter and naming gates, review meaningful whitespace and simple control flow,
and preserve useful comments and error handling. Measure complete client files
including helpers with [the source compactness tool](compactness.md).

There is no weighted score, universal cognitive limit, minimum token reduction,
or invented performance budget. A small source can still be difficult to read.
Human review should identify concrete reasons: clear names, visible lifecycle
steps, understandable configuration, local reasoning, and consistent error paths.

## Cognitive complexity and readability signals

`tools/quality/readability.cmake` invokes an installed
[Clang-tidy cognitive complexity check](https://clang.llvm.org/extra/clang-tidy/checks/readability/function-cognitive-complexity.html)
with the existing compilation database. It needs CMake 3.24 or newer and a
compatible Clang-tidy installation, including C++23 headers for the configured
toolchain. It requires no Python on Windows. Generate `compile_commands.json`
with the project's Ninja configuration and `CMAKE_EXPORT_COMPILE_COMMANDS=ON`;
Visual Studio generators do not supply this database.

In PowerShell, with an existing compatible build:

```powershell
cmake "-DBUILD_DIR=$PWD/build/windows-msvc-release" `
  "-DFILES=$PWD/tools/compactness/scanner.cpp;$PWD/tools/compactness/report.cpp" `
  "-DHEADERS=$PWD/tools/compactness/compactness.hpp" `
  "-DCLANG_TIDY=C:/Program Files/LLVM/bin/clang-tidy.exe" `
  "-DOUTPUT_DIR=$PWD/history/MyFeature/readability-after-1" `
  -P tools/quality/readability.cmake
```

Use the same `cmake -D... -P ...` arguments on Linux, with shell quoting for the
semicolon-separated `FILES` list. `CLANG_TIDY` must be an absolute executable
path when supplied; otherwise the driver searches for `clang-tidy`. Optional
`TIMEOUT_SECONDS` defaults to 120 per subprocess. Sources must have explicit
compilation database entries. Optional `HEADERS` lists explicit headers to include
in the diagnostic scope and SHA-256 receipt; headers need no invented compilation
command and are analyzed through the real TUs. The driver does not switch
compilers to conceal an incompatible build. Canonical duplicate TUs and headers
count once. Main-file and explicitly selected header diagnostics are reported;
other headers are filtered out.

Optional semicolon-separated `EXTRA_ARGS` supplies explicit compiler arguments
through Clang-tidy's `--extra-arg`, for example a toolchain compatibility define.
Arguments are recorded in `extra-args.json` and `status.json`; production build
settings remain untouched. These arguments describe the analysis configuration
and must be considered when comparing receipts. They cannot override the driver's
check configuration directly.

Clang filters file-entry spelling, which may contain relative paths and uncollapsed
`..` segments. The driver builds exact escaped regex alternatives from canonical
header paths and verified aliases through explicit TU/header directories and the
compilation working directory. Every alias resolves to a selected header. The
actual per-TU filter is retained with the raw logs. Unusual include spellings can
require an explicit analysis TU with an absolute header include; an unreported
function is never assumed to have zero complexity. Diagnostics from a shared
header are deduplicated by canonical location/function/complexity across TUs.

Threshold **0** enables reporting of positive cognitive complexity. This is a
measurement setting, not an acceptance threshold. Zero-complexity functions are
not enumerated, and filtered/unsupported functions are not fabricated. The
structured `positive_complexity_diagnostics` count is a count of unique emitted
diagnostics, not a count of all functions or proof of zero issues. Parsed records
include the translation unit, original location-bearing diagnostic, function name,
and complexity. Different observed complexities at one template/macro location
remain distinct observations. The raw diagnostics and fixes YAML remain authoritative.
Template, macro, and language limitations are those of the installed check.

`ENABLE_FUNCTION_SIZE=ON` and `ENABLE_MAGIC_NUMBERS=ON` add the existing
[function-size](https://clang.llvm.org/extra/clang-tidy/checks/readability/function-size.html)
and [magic-numbers](https://clang.llvm.org/extra/clang-tidy/checks/readability/magic-numbers.html)
signals. Their installed defaults are recorded through the tool version and
configuration; they are not newly accepted project limits. They remain raw
diagnostics rather than made-up source quality metrics. Existing formatting and
naming checks stay separate.

Each new output directory retains `status.json`, `version.txt`, `config.json`,
`checks.txt`, explicit source SHA-256 records, the compilation database SHA-256,
and per-TU stdout, stderr, header filters, and exported YAML when emitted. Explicit
header SHA-256 records are also retained. Source/header/database changes
during analysis fail visibly. Hashes cover the explicit inputs, not the entire
include graph or toolchain installation. Source and compilation database files
remain untouched. Existing output directories are never overwritten.

Successful execution returns zero with status `REPORTED`, including a legitimate
run with no positive-complexity diagnostics. Missing/incompatible tooling,
missing commands, unsupported diagnostic spelling, and invalid setup return
nonzero with `NOT_RUN`. Compiler, tool, timeout, and changed-input failures return
nonzero with `FAIL`. Failure receipts and any collected raw evidence remain in
the output directory. An unavailable compiler or check cannot produce a passing
zero-issue report. Lexical brace nesting from the compactness scanner is a
different diagnostic and is never labeled cognitive complexity.

## Performance history

`tools/quality/performance.cmake` archives and compares existing benchmark JSON.
It does not introduce another timing loop, runtime, or benchmark framework.
First build and run the exact executable for each side of a feature change;
retain its exit code and JSON before importing. Use a directory layout such as
`history/MyFeature/before-run-1`, `after-run-1`, and further independent runs.
No directory or comparison file is overwritten. This provides history through
retained artifacts; there is no automatic scheduler, checkout, or Git operation.

Example for the existing managed benchmark, in PowerShell:

```powershell
New-Item -ItemType Directory -Force history/MyFeature | Out-Null
& ./build/Release/portals_managed_benchmark.exe --rows 128 --samples 30 --warmup 5 `
  --output history/MyFeature/raw-after-1.json
$benchmarkExitCode = $LASTEXITCODE
```

Prepare a metadata JSON object with actual provenance for that invocation:

```json
{
  "revision": "actual source revision; include a patch identity for dirty work",
  "toolchain": {
    "compiler": "actual compiler/version",
    "configure_command": "actual configure invocation and flags"
  },
  "configuration": "Release",
  "host": {
    "os": "actual OS/version and host identity",
    "cpu": "actual CPU",
    "power_mode": "actual power mode",
    "background_load": "observed concurrent workload"
  },
  "command": ["actual/path/portals_managed_benchmark.exe", "--rows", "128", "--samples", "30", "--warmup", "5", "--output", "history/MyFeature/raw-after-1.json"],
  "benchmark_exit_code": 0,
  "workload": "managed rows128 samples30 warmup5"
}
```

Replace every descriptive value with the actual evidence; the example is not a
measurement receipt. Record the observed exit code, not an assumed zero. Additional
metadata such as executable SHA-256, source patch SHA-256, build command, and
measurement start/end times is retained when supplied. The script cannot prove
caller-supplied provenance or that an executable was built from the named revision.
Keep provenance truthful and the required behavior checks alongside the history.
When raw JSON supplies compiler/build stamps (managed and MVP application), use
those exact strings for `toolchain.compiler` and `configuration`; contradictory
metadata is refused. Compact and separate-encoding output lack these stamps and
require caller-supplied provenance.

Import and compare:

```powershell
cmake -DMODE=record -DKIND=managed `
  "-DRAW_FILE=$PWD/history/MyFeature/raw-after-1.json" `
  "-DMETADATA_FILE=$PWD/history/MyFeature/metadata-after-1.json" `
  "-DOUTPUT_DIR=$PWD/history/MyFeature/after-run-1" -P tools/quality/performance.cmake

cmake -DMODE=compare "-DBEFORE_DIR=$PWD/history/MyFeature/before-run-1" `
  "-DAFTER_DIR=$PWD/history/MyFeature/after-run-1" `
  "-DOUTPUT_FILE=$PWD/history/MyFeature/comparison-1.json" -P tools/quality/performance.cmake
```

Record mode keeps byte-exact `raw.json`, including formatting, line endings, and
all raw samples exported by the existing benchmark. `record.json` contains schema
1, adapter `portals-performance-1`, UTC import time, raw SHA-256, supplied metadata,
and normalized observations. Import time is not the measurement start time.
The original input is untouched. Compare mode rechecks the raw hash and recomputes
observations rather than trusting cached normalized numbers.

## Supported observations and compatibility

| `KIND` | Existing output and observations |
| --- | --- |
| `managed` | Schema-1 complete/correctness-verified managed benchmark; per API/rows/phase p50/p95 ns, retained min/max/count/raw timing samples, and scene-node context. `--check` is refused as a performance baseline. |
| `compact` | Current compact benchmark stdout; per API/phase p50/p95 ns, ordinary allocation-call min/max, sample count, scene nodes, and column workload. This benchmark exports no raw timing arrays or timing variance; those fields remain absent/null in the evidence. |
| `mvp` application | Schema-1 complete fixed-viewport application observation with successful accounting, OHLC, and shutdown; phase, transport-read, data-ingress, and runner-control p50/p95 with retained range/count/overflow; actual accepted frames, bytes, wall time, and process CPU time as context. |
| `mvp` separate encoding | Schema-1 complete oracle-verified encoding observation; full/diff/render p50/p95, retained ranges/counts/raw samples, and frame/byte context. |

For managed and MVP series, nearest-rank summaries are checked against retained
raw samples. Empty or overflowed MVP series have `NOT_MEASURED` evidence and null
comparison values; retained partial sample ranges remain visible. Never turn a
missing latency phase into zero. Compact allocation counters count ordinary
`new`/`new[]` calls, not bytes, aligned allocation, or malloc. Managed output has
no allocation metric to invent. Timing boundaries and units are described in the
[benchmark README](../tools/benchmarks/README.md) and
[managed measurement documentation](managed-performance.md).

Comparison requires identical benchmark kind, toolchain metadata, configuration,
host metadata, workload label, actual raw-derived workload signature, and metric
shape/units. Managed signatures include compiler, assertion mode, pointer width,
sample/warmup counts, APIs, rows, nodes, and controls. Compact signatures include
sample/warmup/node/column counts and APIs. MVP application signatures include
compiler, platform, transport, rate, seed, rows, frame cap, observation/warmup,
delay, color/text modes, diagnostics, capacity, boundaries, and viewports.
MVP encoding signatures include capture identity, frames, repetitions, and modes.
Revision and executable/output paths in recorded commands may differ. Configure
commands inside toolchain metadata are compared exactly, so consistently record
their invocation context. Mismatched inputs fail instead of quietly producing
an apples-to-oranges ratio. Metadata equality cannot eliminate ambient noise.

Every available metric reports before, after, signed delta, and
`after_over_before`; zero baseline ratios are null. Ratios use six fractional
digits, truncated by integer long division. Nonnegative integer observations
through `2^53-1` are supported; fractional or larger timing/counter fields are
refused explicitly. Existing floating CPU/throughput summaries remain available
in raw JSON and are not reinterpreted as integer metrics.

Increased durations or allocation calls are labeled `CANDIDATE_REGRESSION`.
Decreased observations are labeled `OBSERVED_DECREASE`; node/frame/byte context
does not receive a universal better/worse direction. This is a candidate finding,
not causality or statistical significance. Each comparison contains one
independent invocation per side. Thirty phase samples within one invocation are
not thirty independent runs. Retain repeated runs under controlled conditions,
inspect sample ranges and the existing benchmark's noise limits, and confirm a
candidate before attributing it to a feature. The MVP benchmark's documented
primary profile uses three independent runs per rate; do not replace that policy
with one short fixture observation. Missing historical baselines remain missing.

Errors in setup, metadata, JSON shape, raw-summary consistency, complete/correctness
flags, SHA-256, compatibility, or numeric support produce nonzero `FAIL` diagnostics.
Failed comparisons publish no success report. CMake fixture tests cover managed,
compact, and both MVP adapters, zero/null values, incomplete/overflowed samples,
raw retention/tampering, metadata/workload incompatibility, malformed inputs,
and unavailable/unsupported Clang policy without running slow benchmarks.
Pass an absolute `TEST_CLANG_TIDY` executable to `tests/quality_tools.cmake` to
add live TU/header scope, cross-TU deduplication, and compiler-failure integration;
without it that stage reports `NOT_RUN` explicitly.
