# Managed API performance experiment {#managed-performance}

`portals_managed_benchmark` compares equivalent scene work through the low-level
API, `ManagedScene`, and owning `ui` descriptions. It is a finite, headless
microbenchmark, not an application frame-rate or terminal-throughput benchmark.
The source is `tools/benchmarks/managed.cpp`; the correctness/schema check is
`tools/benchmarks/check_managed.cmake`.

## Reproducing a run

Build the benchmark target with `PORTALS_BUILD_TOOLS=ON` and run the exact
binary from the configuration being reported. For example, in PowerShell with
an existing MSVC multi-configuration build:

```powershell
cmake --build build --config Release --target portals_managed_benchmark
& .\build\Release\portals_managed_benchmark.exe --output managed-release.json
& .\build\Release\portals_managed_benchmark.exe --check
cmake -DPROGRAM="$PWD/build/Release/portals_managed_benchmark.exe" -P tools/benchmarks/check_managed.cmake
```

For a single-configuration build, set `CMAKE_BUILD_TYPE=Release` at configure
time and use the executable directly under that build directory. The default
run uses 32, 128 and 512 rows, five warmup lifecycles and 30 retained lifecycles
per API/size pair. Each lifecycle uses a fresh application and scene. API order
rotates within each round; sizes run in ascending order.

`--rows N` replaces the size list with one size (1–1024). `--samples N` accepts
1–200 retained lifecycles and `--warmup N` accepts 0–50 discarded lifecycles.
`--output PATH` writes JSON; without it, JSON goes to stdout. `--check` overrides
the size/sample/warmup options with two small sizes (2 and 8), three samples and
one warmup. It exercises correctness and aggregation without performance
thresholds. Every run is bounded. Errors produce stderr and a nonzero exit;
failed runs do not publish a new complete result.

For published results, retain the JSON together with the source revision, OS,
CPU, power mode, compiler/configure command, build configuration and other
active workloads. The JSON records compiler version, build configuration when
provided by CMake, assertion mode, pointer width, sample counts and every raw
duration. `assertions_enabled` describes the benchmark translation unit;
runtime correctness checks remain enabled in Release as well.

## Equal work and timing boundaries

Every scene contains a plain ownership root and N independent `Row` children.
Each row contains one `Label` and one `Button`, giving `3N + 1` scene nodes and
`2N` styled controls, plus the common application root. Captions, control
defaults, one callback per button, focus transitions and theme rules agree.
The low-level path owns its binding tokens and calls `syncControl` explicitly;
the other paths delegate those responsibilities to `ManagedScene`.

Descriptions are created and mounted **one row at a time** using the existing
`ui::row` / `ui::mount` interface. This is not one variadic description with
N compile-time children, nor a dynamic-list/reconciliation experiment. The
description path includes construction, string capture, node references,
mounting and destruction of each consumed description. All paths retain the
same observing row/control handles for validation.

| JSON phase | Included work |
| --- | --- |
| `construction` | Ownership root, scene/style owner, widgets, callback registrations, handle storage; description construction and mount where applicable. |
| `initial_style_register` | One batch for all controls under the initial empty sheet, then creation/installation of Modern theme. Low-level uses `bindBatch` + `replace`; managed paths use `theme`, which flushes pending registrations before replacement. |
| `initial_prepare` | First presentation synchronization and focus recovery after registration. |
| `dirty_prepare` | A later preparation after focusing the last button and explicitly invalidating paint. Focus change and invalidation are outside the timed phase. |
| `theme_switch_prepare` | Creation/installation of Turbo theme and subsequent presentation preparation. |
| `remove_rows` | Removal of every row subtree, disconnection of low-level tokens, and router collection of retired callbacks. |
| `cleanup` | Closing/removing the empty ownership root, destroying the scene/style owner, releasing retained handle/token storage, final router collection. |

Application creation/destruction, correctness traversal, button activation,
summary sorting and JSON/file I/O are outside timed regions. Checked API result
handling is inside measured operations in every path. Warmup uses exactly the
same workload and checks, but contributes no retained samples. One retained
lifecycle contributes one duration to each phase; `dirty_prepare` is therefore
a distribution of repeated preparations across fresh scenes, not an idle loop
on one scene. Cleanup happens after explicit row removal; it must not be read
as the cost of destroying a populated form.

Every lifecycle checks tree topology, sibling order, leaf counts, captions,
focus/enabled presentation, and complete computed styles against `resolveStyle`
under both themes. Every button is activated once; the exact callback count is
checked before and after removal. Removed handles must be stale, invocation
must fail with `STALE_NODE`, and the application root must be empty after
cleanup. These are runtime checks, not `assert` expressions erased by Release.

## Reading the measurements

Durations use `std::chrono::steady_clock`, converted to nanoseconds. The JSON
contains nearest-rank p50/p95, minimum, maximum and all raw samples. Nanosecond
units do not imply nanosecond clock resolution. Small samples, scheduler noise,
allocator caches, cache warming, timer overhead and debug-container checks can
dominate short phases; the benchmark does not subtract an estimated overhead.
For 30 samples p95 is the 29th ordered sample. Three-sample `--check` output is
only a correctness check, not useful performance evidence.

Compare distributions from the same machine and configuration first. For the
default fourfold increases in row count, inspect the registration p50/p95
ratios together with raw samples: approximately fourfold growth is consistent
with linear work over that interval; approximately sixteenfold growth is
consistent with quadratic work. Neither establishes asymptotic complexity from
three sizes. A larger absolute managed cost also does not establish a worse
growth rate; report both the timings and ratios.

Batch registration avoids repeatedly recomputing previously registered styles
after every `bind`. It **does not guarantee linear registration**: the current
`StyleEngine::bindBatch` implementation scans preceding registrations to reject
duplicates and searches a growing binding vector. Those identity comparisons
are quadratic for a batch of distinct new nodes. Managed node lookup, router
bookkeeping and per-row removal also contain linear searches; construction and
removal must be measured separately from style registration. The experiment
does not change those contracts or silently optimize the core.

No measured timing or claim of nonquadratic scaling is supplied by the source
alone. Actual runs must be recorded before drawing a performance conclusion.
There is no allocation counter: overriding a subset of allocation operators or
counting objects would not establish complete process allocation behavior.
Layout, painting, terminal I/O, properties, LineEdit synchronization, long-lived
state updates and virtualized model workloads are outside this experiment.
The low-level reference deliberately uses the public batch registration API;
it does not penalize that API with the old repeated-`bind` pattern.

See [managed ownership](managed-controls.md), [owning descriptions](declarative-ui.md)
and [style transactions](styles.md) for the contracts measured here.

## Recorded Release experiment

The following run used an AMD Ryzen 9 5950X, Windows build 26340 with the
High performance power plan, MSVC 19.51, and GCC 14.3 under WSL2 on the same
machine. Builds and sanitizer/naming runs had finished before measurement;
the Windows and WSL benchmarks ran sequentially. This was a desktop session,
not an isolated performance laboratory: other user/system activity was not
stopped or measured. Compare APIs within one compiler run, not the two
compilers as a compiler-speed ranking.

Each API/size pair has 30 retained samples after five warmups. Times below
are **p50 / p95 in milliseconds**. Raw samples, all seven phases and compiler
metadata are available for [MSVC](measurements/managed-msvc-release.json)
and [GCC](measurements/managed-gcc-release.json).

| Compiler | API | Rows | Construction | Style registration | Dirty prepare | Per-row removal |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| MSVC 195136257 | low_level | 32 | 0.135 / 0.157 | 0.043 / 0.054 | 0.009 / 0.009 | 0.098 / 0.120 |
| MSVC 195136257 | managed | 32 | 0.168 / 0.214 | 0.045 / 0.059 | 0.010 / 0.011 | 0.133 / 0.152 |
| MSVC 195136257 | descriptions | 32 | 0.218 / 0.262 | 0.046 / 0.062 | 0.010 / 0.011 | 0.129 / 0.153 |
| MSVC 195136257 | low_level | 128 | 0.848 / 1.154 | 0.176 / 0.203 | 0.049 / 0.059 | 0.508 / 0.802 |
| MSVC 195136257 | managed | 128 | 1.057 / 1.380 | 0.187 / 0.255 | 0.056 / 0.067 | 0.861 / 1.217 |
| MSVC 195136257 | descriptions | 128 | 1.191 / 1.443 | 0.192 / 0.282 | 0.056 / 0.058 | 0.890 / 1.258 |
| MSVC 195136257 | low_level | 512 | 7.683 / 11.560 | 1.299 / 1.818 | 0.558 / 0.874 | 3.612 / 5.385 |
| MSVC 195136257 | managed | 512 | 9.595 / 12.079 | 1.338 / 2.127 | 0.609 / 0.973 | 9.364 / 13.328 |
| MSVC 195136257 | descriptions | 512 | 10.413 / 13.969 | 1.327 / 1.825 | 0.604 / 1.055 | 9.220 / 12.361 |
| GCC 14.3.0 | low_level | 32 | 0.060 / 0.063 | 0.031 / 0.042 | 0.006 / 0.006 | 0.068 / 0.084 |
| GCC 14.3.0 | managed | 32 | 0.070 / 0.096 | 0.032 / 0.040 | 0.007 / 0.008 | 0.085 / 0.101 |
| GCC 14.3.0 | descriptions | 32 | 0.091 / 0.111 | 0.032 / 0.048 | 0.007 / 0.008 | 0.084 / 0.092 |
| GCC 14.3.0 | low_level | 128 | 0.424 / 0.460 | 0.138 / 0.151 | 0.050 / 0.059 | 0.342 / 0.417 |
| GCC 14.3.0 | managed | 128 | 0.499 / 0.520 | 0.144 / 0.166 | 0.056 / 0.068 | 0.576 / 0.713 |
| GCC 14.3.0 | descriptions | 128 | 0.585 / 0.634 | 0.144 / 0.203 | 0.055 / 0.068 | 0.581 / 0.629 |
| GCC 14.3.0 | low_level | 512 | 5.328 / 5.788 | 1.121 / 1.268 | 0.754 / 0.891 | 2.804 / 3.311 |
| GCC 14.3.0 | managed | 512 | 6.451 / 7.003 | 1.244 / 1.442 | 0.827 / 0.990 | 7.066 / 8.065 |
| GCC 14.3.0 | descriptions | 512 | 6.800 / 7.325 | 1.294 / 1.742 | 0.797 / 1.067 | 7.127 / 7.791 |

The next table shows **512-row / 128-row growth ratios (p50 / p95)**
for a fourfold increase in rows. It includes the low-level reference,
so a shared core cost is not confused with description-layer overhead.

| Compiler | API | Construction | Style registration | Dirty prepare | Per-row removal |
| --- | --- | ---: | ---: | ---: | ---: |
| MSVC 195136257 | low_level | 9.06x / 10.01x | 7.37x / 8.94x | 11.32x / 14.84x | 7.12x / 6.71x |
| MSVC 195136257 | managed | 9.08x / 8.76x | 7.17x / 8.35x | 10.89x / 14.54x | 10.87x / 10.95x |
| MSVC 195136257 | descriptions | 8.74x / 9.68x | 6.90x / 6.46x | 10.88x / 18.19x | 10.36x / 9.83x |
| GCC 14.3.0 | low_level | 12.58x / 12.58x | 8.13x / 8.38x | 14.98x / 15.19x | 8.20x / 7.95x |
| GCC 14.3.0 | managed | 12.92x / 13.45x | 8.62x / 8.70x | 14.79x / 14.56x | 12.26x / 11.30x |
| GCC 14.3.0 | descriptions | 11.62x / 11.56x | 8.96x / 8.58x | 14.48x / 15.80x | 12.26x / 12.39x |

At 512 rows (1,537 scene nodes), the description/low-level p50 ratios were:

- MSVC 195136257: construction 1.36x, initial_style_register 1.02x, dirty_prepare 1.08x, remove_rows 2.55x.
- GCC 14.3.0: construction 1.28x, initial_style_register 1.15x, dirty_prepare 1.06x, remove_rows 2.54x.

Construction is paid during mounting; these are not per-frame painting costs.
The observed growth exceeds fourfold in construction, registration, dirty
preparation and per-row removal, including the low-level reference. A small
relative dirty-prepare overhead therefore does **not** establish linear work
at a dirty-frame boundary. These measurements do not satisfy a
no-quadratic-registration goal, nor certify scalable bulk creation/removal.
They support trying the additive API on ordinary static forms while exposing
the shared core costs that need a separate optimization and regression check.

Before advertising broader scaling guarantees, address style identity lookup,
managed/router searches and repeated per-subtree cleanup, then rerun the same
workload. Virtual-view lifecycle remains outside this experiment. No timings
are CI acceptance thresholds.

The measured baseline is commit `0276ae2` plus the palette/example/benchmark
changes described here. The measured `tools/benchmarks/managed.cpp` SHA-256 is
`42a4f8f60a670f778919c4c5b0ef820dcece3cd73c52170817707b1ec02c061f`. Lifecycle core and owning-description code match that
baseline; the palette additions do not change `bindBatch` or its algorithms.
