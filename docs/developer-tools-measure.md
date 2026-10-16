# Measurement and terminal tools {#developer-tools-measure}

[Русская версия](ru/developer-tools-measure.md) · [Tool catalog](developer-tools.md)

<a id="tool-change-impact"></a>
## Measure includes and rebuild footprint

`tools/quality/change_impact.py` reads the dependencies recorded by an existing
Ninja build. It identifies observed affected compile outputs, translation units
and CMake targets for each `--changed` file. It never builds or selects CI jobs.
Python 3.10+ with the standard library is an authoring dependency; SDK consumers
do not need it. Ninja's [tool reference](https://ninja-build.org/manual.html#_extra_tools)
defines `-t deps` and `-t compdb`.

```powershell
python tools/quality/change_impact.py --help
python tools/quality/change_impact.py --build-dir build/tools --changed include/portals/managed_diagnostics.hpp --output-dir build/impact-001
python tools/quality/change_impact.py --deps build/impact-001/ninja-deps.txt --compdb build/impact-001/ninja-compdb.json --recorded-source-root C:/src/portals --changed include/portals/style.hpp
python tools/quality/test_change_impact.py
```

The offline example assumes the capture was built from `C:/src/portals`; supply
the actual root spelling in your records. Use a new output directory for each
receipt. Ordinary `compile_commands.json` can lack `output`; this tool requires
Ninja's `-t compdb` format and rejects records without that field.

| Argument | Meaning | Default |
| --- | --- | --- |
| `--build-dir DIR` | Existing Ninja build; read-only `-t deps` and `-t compdb` | No default; choose this or `--deps` |
| `--deps FILE` | Recorded UTF-8 dependency capture; requires `--compdb` and `--recorded-source-root` | No default |
| `--compdb FILE` | Recorded Ninja JSON with `directory`, `file` and `output` | No default |
| `--source-root DIR` | Checkout scanned for the separate lexical public include graph | Repository containing the script |
| `--recorded-source-root PATH` | Absolute source-root spelling in captures, including a different host's Windows/POSIX paths | Live: `--source-root`; offline: required |
| `--source-revision LABEL` | Caller-declared source revision; the analyzer does not verify this label | `unknown` |
| `--changed FILE` | Source-relative or absolute changed path; repeatable | No impact queries |
| `--ninja FILE` | Installed Ninja executable, live mode only | `ninja` from PATH |
| `--output-dir DIR` | New receipt directory; existing paths are rejected | JSON stdout only |

There are no timing units or workload limits. Exit 0 means analysis was produced,
including a partial/unknown result; 1 means malformed input, I/O or Ninja failure;
2 means invalid arguments. Help needs no operational inputs and writes nothing.
An output directory contains `impact.json`, `public-includes.dot`,
`observed-headers.dot`, `subsystems.dot`, `summary.md` and both raw captures.
Existing receipts and inputs are never overwritten. An I/O failure may leave a
partial new directory. JSON includes capture hashes and per-header source hashes.
No source, build output, Git configuration or running process is changed.
Graphviz is optional for viewing DOT; the JSON and Markdown need no renderer.

Two different graphs remain explicit. Ninja dependencies are transitive
observations from the last build. Literal public `#include` edges come from a
lexical scan of `--source-root`; they include inactive conditional branches and
do not resolve macro includes or module imports. `subsystems.dot` groups observed
paths by containing directory; these names do not define architectural roles.
Live mode checks observed object/dependency modification times. Offline captures
cannot establish current freshness. Keep the scanned checkout consistent with
the captured revision; the declared revision label is not a verification step.

Coverage reports unbuilt compile outputs, stale/empty records, unsupported module
sources and unknown dependency records. Linker and phony compdb rows are excluded;
linker dependency records are reported separately. Windows paths are normalized
case-insensitively; case-sensitive Windows directories are outside this model.
CMake target names come from `CMakeFiles/<target>.dir` paths, preserving their
spelling; other generators or output layouts can leave a target unknown.
An unobserved changed file has null impact, never a claimed zero. Missing coverage
or freshness returns a full/unknown recommendation. Even complete observed
coverage describes only that configuration and retains required checks.

### Recorded baseline and the diagnostic/style boundary

The [numeric baseline](measurements/include-impact-968651a.json) describes
source `968651a90b6a1542fd3ab2cec159b457e548681f`, Ninja Release, GCC C++ 14.3.0
and C 13.3.0, `-O3 -DNDEBUG`, tools enabled and tests/examples/docs disabled.
Only core and selected quality/benchmark targets were built. It records 46
compile outputs from 51 configured compile rows; five outputs were unbuilt.
The 50 raw Ninja records also include four linker records. These counts are
dependency footprint, not measured compile time or a speed improvement.

| Measurement | Observed value |
| --- | ---: |
| Compiler-observed public headers | 57 |
| Public headers in the separate lexical scan | 61 |
| Literal public include edges | 99 |
| `geometry.hpp` affected compile outputs | 34 |
| `text.hpp` affected compile outputs | 32 |
| `managed_diagnostics.hpp` affected compile outputs | 7 |
| `style.hpp` / `detail/pss_data.hpp` affected compile outputs | 9 / 9 |
| `pss_file_error.hpp` affected compile outputs | 7 |

The baseline [public include graph](measurements/include-impact-968651a.dot)
and [directory-group graph](measurements/include-subsystems-968651a.dot) are DOT
text, with the full relative-path edges also in JSON. The
[diagnostic header](../include/portals/managed_diagnostics.hpp) directly includes
style, PSS data and file-error categories. The observed fan-in quantifies the
current boundary; it does not justify moving error variants or predict the
benefit of a header split. Such changes need their own contracts and measurements.

`test_change_impact.py` is an internal unittest driver: it creates temporary
fixtures and checks parser failures, stale/unbuilt coverage, Windows/POSIX paths
with spaces, help side effects, deterministic receipts and preservation of prior
outputs. It returns unittest status and invokes no compiler or real build.

## Build the executables

Use CMake 3.24+, Ninja and a C99/C++23 toolchain. On Windows use an x64 Developer
PowerShell. This creates a separate Release build and does not publish anything:

```powershell
cmake -S . -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DPORTALS_BUILD_TOOLS=ON -DPORTALS_BUILD_TESTS=OFF -DPORTALS_BUILD_EXAMPLES=OFF -DPORTALS_BUILD_DOCS=OFF
cmake --build build/tools
```

Commands below use Windows executable paths. On POSIX omit `.exe`. With a
multi-configuration generator select Release at build time and use its subdirectory.
Create the output directory before running a benchmark. A benchmark `--output`
file can be overwritten; choose a fresh path when retaining observations.

<a id="tool-compactness"></a>
## Compare source files

`portals_compactness` reads the explicitly selected C++ files, deduplicates canonical
paths on each side and writes a report to stdout. It does not modify inputs or
check runtime behavior. [Source](../tools/compactness/report.cpp),
[measurement definitions](compactness.md).

| Argument | Meaning | Default |
| --- | --- | --- |
| `--before FILE` | Required baseline file; repeat to include helpers | None |
| `--after FILE` | Required candidate file; repeat to include helpers | None |
| `--format json\|markdown` | Output format; specify at most once | `json` |
| `--help` | Print usage; use on its own | Off |

```powershell
./build/tools/portals_compactness.exe --before docs/examples/docs_basic.cpp --after docs/examples/docs_events.cpp --format markdown
```

Exit 0 means a report or help was printed; exit 1 means an invalid argument,
read/scan failure. This example compares different programs;
its counts do not establish equivalent behavior.

<a id="tool-benchmarks"></a>
## Run a benchmark

All five executables below build with `PORTALS_BUILD_TOOLS=ON`. Only the MVP
`--transport native` mode opens a terminal. The others run in process. Each benchmark supports standalone `--help` or `-h`, which prints usage and
returns 0 before workload, terminal or output creation. Mixing a help flag with
other arguments returns 1. Other option parsing is unchanged; the compact
benchmark has no workload options.

### Managed construction and style layers

[managed.cpp](../tools/benchmarks/managed.cpp) compares three construction APIs.
[style_layers.cpp](../tools/benchmarks/style_layers.cpp) measures equal, masked
and visible style changes across visible, hidden and off-screen controls.
They share this CLI:

| Argument | Accepted values | Default |
| --- | --- | --- |
| `--rows N` | `1..1024`; one row count for managed, one control count for style layers | Separate runs at `32,128,512` |
| `--samples N` | `1..200` retained samples per case | `30` |
| `--warmup N` | `0..50` untimed samples per case | `5` |
| `--output FILE` | JSON destination in an existing directory | stdout |
| `--check` | Finite correctness check; overrides rows/samples/warmup with `2,8` / `3` / `1` | Off |

```powershell
./build/tools/portals_managed_benchmark.exe --check
./build/tools/portals_style_layers_benchmark.exe --check
./build/tools/portals_managed_benchmark.exe --rows 128 --samples 30 --warmup 5 --output managed.json
```

Exit 0 means success; exit 1 means invalid options, a failed invariant or export
failure. These programs use fixed workloads; [managed performance](managed-performance.md)
explains what their observations support.

### PSS parsing

[pss.cpp](../tools/benchmarks/pss.cpp) measures seven runtime parser cases.

| Argument | Accepted values | Default |
| --- | --- | --- |
| `--samples N` | `1..10000` retained timed batches per case | `100` |
| `--warmup N` | `0..1000` untimed batches | `10` |
| `--batch N` | `1..1024` individually timed parses averaged in each batch | `16` |
| `--reverse` | Reverse case order | Off |
| `--check` | Validate results without collecting timing | Off |
| `--output FILE` | JSON file in an existing directory | stdout |

```powershell
./build/tools/portals_pss_benchmark.exe --check
./build/tools/portals_pss_benchmark.exe --samples 100 --warmup 10 --batch 16 --output parse.json
```

Exit 0 means success, exit 1 means argument, correctness or export failure.
Percentiles describe batch averages. Successful-result destruction is outside
timing; partial-error cleanup and clock overhead are included.

### Compact construction

[compact.cpp](../tools/benchmarks/compact.cpp) has no parsed arguments. Run
`./build/tools/portals_compact_benchmark.exe`; JSON goes to stdout. It uses three
warmups and 30 samples. Exit 0 means success, exit 1 means a caught failure.
It measures fixed scene and column cases; it does not inspect your application's
source. See [benchmark methodology](../tools/benchmarks/README.md).

### MVP application and encoding

[mvp.cpp](../tools/benchmarks/mvp.cpp) runs the dashboard through `Application`.
An output file is required, including in encoding-only mode.

| Argument | Accepted values / meaning | Default |
| --- | --- | --- |
| `--output FILE` | Required JSON file; parent directory must exist | None |
| `--transport headless\|native` | In-process transport or real terminal | `headless` |
| `--rate N` | `0..1000000` requested ticks/second | `1000` |
| `--seed N` | Unsigned 64-bit deterministic input seed | `42` |
| `--rows N` | `1..1000000000` model rows | `1000000` |
| `--fps N` | `1..1000` frame cap | `30` |
| `--warmup-ms N` | `0..3600000` warmup milliseconds | `5000` |
| `--duration-ms N` | `1..3600000` observation milliseconds, excluding warmup | `30000` |
| `--delay-ms N` | Unsigned 64-bit provider delay in milliseconds | `0` |
| `--sample-capacity N` | `1..1000000` retained values per series | `65536` |
| `--diagnostics enabled\|disabled` | Runtime diagnostic collection | `disabled` |
| `--ascii` | Use ASCII workload text | Off |
| `--ansi16` | Use 16 ANSI colors | Off |
| `--encoding-only` | Finite replay measuring full/diff encoding | Off |

```powershell
./build/tools/portals_mvp_benchmark.exe --transport headless --warmup-ms 200 --duration-ms 500 --output mvp.json
```

Exit 0 means a complete successful observation; exit 2 means orderly early
shutdown with an incomplete observation; exit 1 means failure. Encoding mode
uses its own finite replay instead of the timed application observation.
Native mode consumes terminal input and temporarily changes modes. Do not run
it on handles shared with another UI. The [methodology](../tools/benchmarks/README.md)
describes viewport, warmup, raw samples and repeat-run requirements; the short
command above is a functional check, not a performance baseline.

### Benchmark test drivers

`tools/benchmarks/check.cmake` is the internal `portals.mvp-measurement` CTest
driver. CMake supplies `PROGRAM` and `OUTPUT_DIRECTORY`; it writes three `mvp-*.json`
files and gives each child 20 seconds. `check_managed.cmake` is the
`portals.managed-measurement` driver: CMake supplies `PROGRAM`; its normal check has
a 30-second child timeout, and it also tests output and invalid arguments.
Run these through CTest in a build with tests and tools enabled. They are
correctness checks, not baseline collectors. CMake failures return nonzero.

`readability.cmake` and `performance.cmake` accept `-DHELP=ON`
(default `OFF`): embedded help needs no required inputs and returns `0` before
creating files or running analyzers.

<a id="tool-readability"></a>
## Collect readability observations

`tools/quality/readability.cmake` requires CMake 3.24+, an installed compatible
Clang-tidy, and a Ninja compilation database generated with
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`. It creates a fresh receipt directory, launches
Clang-tidy and leaves sources and compiler settings unchanged.

| CMake variable | Meaning | Default |
| --- | --- | --- |
| `BUILD_DIR` | Required directory containing `compile_commands.json` | None |
| `FILES` | Required semicolon-separated translation units in the database | None |
| `OUTPUT_DIR` | Required new result directory; must not exist | None |
| `HEADERS` | Explicit headers included in scope and hashes | Empty |
| `CLANG_TIDY` | Absolute executable path | Search `PATH` |
| `TIMEOUT_SECONDS` | Integer `>=1`, per subprocess | `120` |
| `EXTRA_ARGS` | Semicolon-separated compiler arguments | Empty |
| `ENABLE_FUNCTION_SIZE` | Add the installed function-size check | Off |
| `ENABLE_MAGIC_NUMBERS` | Add the installed magic-numbers check | Off |

```powershell
cmake "-DBUILD_DIR=$PWD/build/tools" "-DFILES=$PWD/tools/compactness/scanner.cpp" "-DOUTPUT_DIR=$PWD/readability-run" -P tools/quality/readability.cmake
```

The example requires a compatible compilation database in that build. Output
includes `status.json`, configuration, input hashes and per-TU logs/YAML.
Exit 0 means a completed `REPORTED` run; unavailable or failed analysis returns
nonzero with `NOT_RUN` or `FAIL` evidence when the receipt can be created.
Positive cognitive complexity is reported at threshold 0; no acceptance limit
is imposed. See [full interpretation and scope](quality.md).

<a id="tool-performance"></a>
## Archive and compare performance

`tools/quality/performance.cmake` processes existing JSON; it does not run a
benchmark. It requires CMake 3.24+.

| CMake variable | Required in | Meaning |
| --- | --- | --- |
| `MODE` | Both | `record` or `compare` |
| `KIND` | `record` | `managed`, `compact`, `mvp` or `pss`; style-layer JSON has no adapter |
| `RAW_FILE` | `record` | Existing raw benchmark JSON |
| `METADATA_FILE` | `record` | Existing provenance JSON with the schema described below |
| `OUTPUT_DIR` | `record` | New archive directory; must not exist |
| `BEFORE_DIR` | `compare` | Baseline archive created by `record` |
| `AFTER_DIR` | `compare` | Candidate archive created by `record` |
| `OUTPUT_FILE` | `compare` | New comparison JSON; must not exist |

All listed variables have no default. The [quality guide](quality.md) gives the
metadata schema and complete record example. Preserve toolchain, configuration,
host, workload and executed-command provenance with the raw observation.

```powershell
cmake -DMODE=record -DKIND=managed -DRAW_FILE=managed.json -DMETADATA_FILE=metadata.json -DOUTPUT_DIR=history/managed-before -P tools/quality/performance.cmake
cmake -DMODE=compare -DBEFORE_DIR=history/managed-before -DAFTER_DIR=history/managed-after -DOUTPUT_FILE=history/comparison.json -P tools/quality/performance.cmake
```

The first command assumes a valid `metadata.json`; the second assumes two
compatible recorded runs. `record` writes `raw.json` and `record.json`. `compare`
writes signed differences and ratios only after checking compatibility and hashes.
Exit 0 means completion; invalid, incompatible or incomplete input returns nonzero.
A candidate regression is an observation requiring repeated runs, not proof of causality.

<a id="tool-probe"></a>
## Inspect a terminal

`portals_terminal_probe` prints escaped `key="value"` lines. Its
[complete guide](terminal-probe.md) describes the query and restoration limits.

| Argument | Accepted values / meaning | Default |
| --- | --- | --- |
| `--help` | Print usage | Off |
| `--no-query`, `--headless` | Inspect without reading input or changing modes | Off |
| `--timeout-ms N` | `1..10000`, one query-window budget | `250` |
| `--max-bytes N` | `1..65536`, input retention limit | `4096` |
| `--width-policy narrow\|wide` | Advisory ambiguous-width policy | `narrow` |

```powershell
./build/tools/portals_terminal_probe.exe --headless
```

Redirected input or output automatically skips queries. Native query mode consumes
input and restores its temporary modes before returning. Exit 0 means a completed
observation, including skipped or unanswered queries; exit 1 means I/O, restore
or output failure; exit 2 means invalid options. Missing replies do not prove that
a capability is unavailable.

<a id="tool-native"></a>
## Check native terminal cleanup

These launchers run an existing supervisor and child in a separate terminal,
collect logs and check cleanup. Build `portals_terminal_cleanup` and
the selected example in a test-enabled build; see [testing](testing.md).
They create processes and log files, and can inject fixture input.

### Windows launcher

[tools/native_windows.ps1](../tools/native_windows.ps1) requires Windows PowerShell,
Windows Terminal by default, and the built supervisor and child.

| Parameter | Meaning | Default |
| --- | --- | --- |
| `-Supervisor PATH` | Required supervisor executable | None |
| `-Child PATH` | Required child executable | None |
| `-LogDirectory PATH` | Required new directory; must not exist | None |
| `-ChildArguments ARRAY` | Arguments forwarded to child | Empty |
| `-Drive none\|dashboard` | Supervisor input scenario | `none` |
| `-HostWheel` | Submit a wheel event to the uniquely identified fixture window | Off |
| `-ExpectedExit N` | Expected child exit code | `0` |
| `-DeadlineMs N` | Supervisor budget in milliseconds | `15000` |
| `-QuitMs N` | Scheduled quit time in milliseconds | `4500` |
| `-Terminal PATH` | Terminal executable | `wt.exe` |

```powershell
powershell -NoProfile -File tools/native_windows.ps1 -Supervisor ./build/tests/portals_terminal_cleanup.exe -Child ./build/tests/portals_gallery.exe -LogDirectory ./native-run -Drive dashboard
```

Paths above assume those executables have been built in `build/tests`. Run `Get-Help ./tools/native_windows.ps1 -Full` for its embedded reference. The script accepts integer timing arguments without its own
range validation; the supervisor validates them. It writes `supervisor.log`,
`runtime.json`, `baseline.json`, `hashes.json`, `after.json`, `launch.json` and optional
`host-input.json`. Exit 0 requires supervisor success, fixture termination and
preservation of previously running terminal processes; failed checks return 1.
Setup exceptions also fail. Forced cleanup is failure evidence. Host wheel
submission alone does not establish that the application handled the event.

### POSIX xterm launcher

[tools/native_xterm.sh](../tools/native_xterm.sh) is POSIX shell; pass its eight
positional arguments in this order, then optional child arguments:

```sh
sh tools/native_xterm.sh xterm ./build/tests/portals_terminal_cleanup ./native-run 15000 0 dashboard 4500 ./build/tests/portals_gallery
```

| Position | Meaning | Default |
| --- | --- | --- |
| 1 `XTERM` | xterm executable | None |
| 2 `SUPERVISOR` | Supervisor executable | None |
| 3 `LOGDIR` | New log directory | None |
| 4 `DEADLINE_MS` | Decimal supervisor deadline, milliseconds | None |
| 5 `EXPECTED_EXIT` | Expected child exit code | None |
| 6 `DRIVE` | `none` or `dashboard` | None |
| 7 `QUIT_MS` | Scheduled quit, milliseconds | None |
| 8 `CHILD` | Child executable; remaining arguments are forwarded | None |

Default `PORTALS_NATIVE_VISIBLE=0` runs with an isolated Xvfb server and private
Xauthority. This needs `Xvfb`, `xauth` and `xterm`, plus the standard shell utilities used by the script (`realpath`, `sha256sum`, `od`, `sed`, `grep`, `mktemp`, `date`). Supply executable paths for xterm and the supervisor.
`PORTALS_NATIVE_VISIBLE=1` uses the existing display and accepts only `DRIVE=none`.
Other visibility values are rejected. `PORTALS_XVFB` overrides the server executable (default `Xvfb`); `PORTALS_XVFB_WORKDIR` optionally changes its working directory. `TMPDIR` selects the private temporary directory parent (default `/tmp`). The visible mode uses the caller’s `DISPLAY` and `XAUTHORITY`. The script records launch state, xterm stdout/stderr
and supervisor output, and cleans up its own created processes and private server.
Exit 0 means success; exit 1 means a failed completed check; invalid arguments can
return 2, and shell/setup failures return nonzero. Standalone `--help` prints usage before any terminal, process or file work.
