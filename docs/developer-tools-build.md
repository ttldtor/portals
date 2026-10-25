# Build and documentation tools {#developer-tools-build}

[Русская версия](ru/developer-tools-build.md)

This reference covers naming checks, CI check selection and generated HTML.
Run the PowerShell commands from the repository root. Use Python 3.10 or newer;
these Python tools use the standard library. Paths below are examples and may
be replaced with your build directories.

## APT download cache {#tool-apt-cache}

`tools/ci/apt_cache.py DIRECTORY` validates disposable `.deb` downloads for the
Ubuntu naming job. It requires `apt-cache` and freshly updated, signed APT indices.
It removes archives with missing metadata, mismatched package/version/architecture
or incorrect SHA256. It does not install packages or change process environment.
Only regular matching archives are retained; symbolic links are removed without
following them. Unrelated files are preserved. Run before installation, while
the cache directory has no concurrent writer.

```powershell
python tools/ci/apt_cache.py --help
python -m unittest discover -s tools/ci -p test_apt_cache.py -v
```

`DIRECTORY` is required and must exist; there is no default directory. The command
prints verified/discarded counts. Exit codes: 0 scan completed, 1 I/O or tool failure,
2 invalid arguments. `--help` needs no cache directory or APT. Cache contents are
untrusted until checked against APT metadata; they do not replace signed repository
verification. This helper is for disposable CI download directories.

<a id="tool-architecture"></a>
## Architecture snapshot exports

`tools/docs/architecture.py` generates SVG in EN/RU and a self-contained offline
HTML viewer from a snapshot's `maps.json` and the checked-in viewer template.
Python 3.10+ with its standard library is an authoring dependency only. Reading
the exports needs no server or package installation. See the
[snapshot maintenance process](architecture.md).

```powershell
python tools/docs/architecture.py --help
python tools/docs/architecture.py
python tools/docs/architecture.py --check
python tools/docs/architecture.py --source docs/architecture/snapshots/m1-968651a/maps.json --check
python tools/docs/architecture_test.py
```

`--source` selects an existing UTF-8 JSON manifest; by default it selects
`docs/architecture/snapshots/m1-968651a/maps.json` relative to this repository,
regardless of the working directory. Generation replaces only generated
`<map>-<language>.svg` and `index.html` beside that manifest. Preserve frozen
snapshots by creating a new directory before changing the data. `--check`
validates public repository path syntax, positive integer line numbers and exact
export bytes without writing. Historical line bounds and anchors are checked
against their pinned Git revisions when those revisions are available; source
archives do not compare historical coordinates with current files.
There are no units or workload parameters. Exit status is 0 for success,
1 for invalid data, I/O failure or stale exports, and 2 for invalid CLI syntax.
`--help`/`-h` needs no manifest and performs no workload or writes. The exporter
does not use Git or network and does not verify that a source line expresses the
claimed contract; pinned-revision checks and fact review supply that evidence.

`architecture_test.py` is an internal regression driver invoked by the command
above. It checks side-effect-free help in an empty directory, deterministic
exports, source references against the pinned Git revision, SVG geometry and
offline resources. It returns the standard unittest success/failure status and
uses temporary directories for CLI checks. Browser and independent fact review
remain separate checks.

<a id="tool-check-plan"></a>
## Plan local contract checks

`tools/ci/check_plan.py` reads explicit change records and a captured CTest
inventory. It prints a JSON check plan; it executes no Git, tests, builds,
compiler, network or external application. Python 3.10+ standard library is an
authoring dependency only. See the [task/integration/milestone matrix](development.md).
The [audited contract registry](../tools/ci/check_contracts.json) maps exact paths
to named CTest registrations, source witnesses and cross-contract focus checks.
This is a bounded registry; it does not claim to classify every project file.
A source witness is an existing test source that supports the contract/test
mapping. Its byte hash makes the cited version auditable; the hash does not
prove that the behavior passed.

```powershell
ctest --test-dir build/windows-msvc-debug --show-only=json-v1 | Set-Content -Encoding utf8 build/check-inventory.json
@'
[{"path":"tools/ci/checks.py","status":"M"}]
'@ | Set-Content -Encoding utf8 build/changes.json
python tools/ci/check_plan.py --changes build/changes.json --inventory build/check-inventory.json
python tools/ci/check_plan.py --changes build/changes.json --inventory build/check-inventory.json --level integration
python tools/ci/check_plan.py --help
python tools/ci/test_check_plan.py
```

Capture the inventory from your chosen configured build. On POSIX use normal
stdout redirection to capture `ctest --show-only=json-v1`. UTF-8 with an optional
BOM is accepted. Change input is an array of `{path,status}` objects. Paths must
be normalized repository-relative POSIX paths, including on Windows. Only `M`
(modified) can qualify for narrow task preflight. Supply added/deleted/copied
files as `A`/`D`/`C`; include both paths for a rename with `R`. These and unknown
statuses force full. The planner trusts the declared complete change list; it
does not discover Git status or validate a caller's `M` declaration.

| Argument | Meaning | Default |
| --- | --- | --- |
| `--changes FILE` | Existing UTF-8 JSON change records | Required |
| `--inventory FILE` | Existing CTest JSON inventory | Absent: full with unknown configured test set |
| `--impact FILE` | Optional [Ninja dependency receipt](developer-tools-measure.md#tool-change-impact) | No dependency evidence |
| `--source-root DIR` | Checkout used to check exact changed paths and source witnesses | Repository containing the script |
| `--level LEVEL` | `task`, `integration` or `milestone` | `task` |
| `--output FILE` | New JSON file; parent must exist, existing files are rejected | stdout only |

There are no timing units or execution parameters. Exit 0 means a plan was
produced, including a full fallback; 1 means invalid data/I/O; 2 means invalid
arguments. `--help`/`-h` needs no operational inputs and writes nothing. Apart
from an explicitly requested new output file, inputs and the environment remain
unchanged. Source archives need neither `.git` nor private workspace data.

`focus_tests` is an ordering aid, with exact source witnesses and byte hashes.
`selected_tests` names actual registrations in the supplied inventory. A missing
inventory produces null, never a fabricated empty set. Full plans supply no
CTest filter; documentation-only plans supply no CTest command. A focused task
plan supplies an exact-name regex. The caller chooses build directory, compiler,
configuration and whether to execute it; a plan is not a PASS receipt. Policy
and inventory hashes identify canonical JSON content; witness hashes identify
source bytes.

The pilot permits a narrow task preflight only for explicitly mapped modified
Python tools with all registrations and witnesses present. C++ core, templates,
styles, consumers and runtime always retain the full configured suite. Include
graphs show rebuild dependencies, not semantic test coverage. Missing, stale,
unbuilt or unobserved dependency coverage is recorded and cannot authorize
skipping; even complete dependency coverage does not qualify runtime exclusions
in this pilot. Unknown/new/deleted/renamed paths, missing witnesses or missing
CTest names also give full.

The JSON retains integration and milestone obligations and reports the hosted
mode from the unchanged `checks.py` selector. A narrow Python task preflight
still requires full hosted checks for infrastructure changes. The existing
docs-only hosted classification is preserved. Integration/milestone plans
select every configured test; applicability of broader platform/native/docs
checks follows the verification matrix. No hosted runtime reduction or measured
time saving is claimed.

The [recorded pilot](measurements/check-plan-pilot-968651a.json) uses 136 actual
CTest registrations from a Ninja Release configuration with tests enabled and
tools/examples/docs disabled. Fourteen hypothetical declared-change scenarios
produce 42 CLI plans across the three levels: 39 full plans retain all 136 tests,
two Python task preflights select two and one registered fixtures, and one
docs-only task plan has no CTest command. There are no unexpected omissions
against these asserted expectations. The runtime source baseline is
`968651a90b6a1542fd3ab2cec159b457e548681f`; the record hashes the planner/matrix
working-tree additions separately. The dependency input covers 46 of 51 compile
outputs and cannot justify a full-project exclusion. The pilot executes plans,
not runtime tests or hosted jobs, and measures no elapsed-time improvement.

`test_check_plan.py` is an internal unittest driver invoked above. It creates
temporary fixtures/source bundles and tests invalid/new/unknown/stale inputs,
cross-contract closure, full-suite preservation, deterministic plans, native
help, BOM input and no-overwrite output. It runs without `.git`, compilers or
runtime tests and returns unittest status. `portals.ci-selection` registers the
existing selector/gate fixtures; `portals.check-plan` registers this driver.

## Choose a command

| Task | Command or target | Changes |
|---|---|---|
| Check C++ names | `python tools/check_naming.py <build>` | Runs clang-tidy; does not request fixes |
| Determine required PR checks | `python tools/ci/checks.py select --base <base> --head <head>` | Reads Git; optionally appends CI output files |
| Validate completed CI jobs | `python tools/ci/checks.py gate` | Reads `CI_NEEDS` |
| Build the complete HTML documentation | `cmake --build <build> --target portals_docs` | Generates and finalizes the HTML tree |
| Check generated links | `python tools/docs/check_links.py <html>` | Reads HTML and referenced local paths |
| Test the CI selector | `python -m unittest discover -s tools/ci -p test_checks.py -v` | Creates temporary Git repositories |
| Test HTML processing | `python -m unittest discover -s tools/docs -p "test_*.py" -v` | Creates temporary fixture files |

The [development guide](development.md) describes contributor workflows;
[testing](testing.md) covers the C++ suite and documentation build setup.

## Build and run the headless suite

Use an x64 Developer PowerShell on Windows, or a compatible C99/C++23 compiler
environment on POSIX, with CMake 3.24+ and Ninja. These commands use a separate
Release build directory:

```powershell
cmake -S . -B build/check -G Ninja -DCMAKE_BUILD_TYPE=Release -DPORTALS_BUILD_TESTS=ON -DPORTALS_BUILD_TOOLS=ON -DPORTALS_BUILD_EXAMPLES=ON -DPORTALS_BUILD_DOCS=OFF
cmake --build build/check
ctest --test-dir build/check --output-on-failure -E '^portals\.line-edit\.native$'
```

The excluded test requires interactive native-terminal input. Run native cleanup
checks separately as described in [terminal tools](developer-tools-measure.md#tool-native).
CTest reports individual failures and returns nonzero when checks fail.

<a id="tool-naming"></a>

## Naming checker

[Source: check_naming.py](../tools/check_naming.py).

Requires a configured `compile_commands.json`, a clang-tidy executable compatible
with the configured compiler and standard library, and the repository's
`.clang-tidy`. Run in the compiler environment used to configure the build.
For example, with Ninja available:

```powershell
cmake -S . -B build/naming -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
python tools/check_naming.py build/naming --clang-tidy clang-tidy
```

| Argument | Required/default | Meaning |
|---|---|---|
| `build` | Required positional path | Directory containing `compile_commands.json`; resolved from the current directory |
| `--clang-tidy EXE` | `clang-tidy` | Executable name on `PATH` or path to the executable |
| `--jobs N` | `1` | Positive maximum number of concurrent clang-tidy processes |
| `--timings JSON` | No report | Overwrite a JSON report at this path; its parent directory must exist |
| `-h`, `--help` | Optional | Print usage and exit |

The script selects unique translation units under `src`, `tests`, `tools`,
`examples`, `experiments` and `fuzz` in this checkout. Files outside the checkout
are skipped. The deliberately invalid `static_scene`, `static_styles` and
`ct_pss` fixtures named `tests/fixtures/<fixture>/invalid.cpp` are also skipped.
Only translation units present in the database are checked.

For each file, it invokes clang-tidy with `-p <build>`, `--quiet` and the
repository `.clang-tidy` as `--config-file`, using the repository root as its
working directory. There is no `--fix` option. Diagnostics are grouped by
translation unit in sorted source order, with stdout and stderr retained on
their respective console streams. Output is spooled to temporary files for at
most `--jobs` units, replayed without loading the whole output into memory, and
removed on exit. A slow earlier unit can delay displaying later diagnostics
and starting the next unit. The final line reports checked and failed units.
The script reads no dedicated environment variables; executable
lookup and compiler discovery still depend on the process environment.

Exit codes: `0` if every invocation succeeds, `1` if any clang-tidy invocation
fails, `2` for invalid arguments or a database with no selected sources.
Unreadable or malformed JSON and process-launch errors terminate unsuccessfully
with a Python diagnostic. The script continues after a clang-tidy nonzero exit
to check the remaining files.

Use `--jobs 2` to allow two checks at once; the CI and release naming jobs use
this setting while retaining the complete selection. With `--timings timings.json`,
the report contains `jobs`, `total_seconds` and a sorted `translation_units`
array of `source` (checkout-relative path), `seconds` and `returncode`.
Times use a monotonic wall clock: per-unit time covers process launch through
completion; total time includes database loading, scheduling and console replay,
but excludes writing the report. Timings are written after all checks finish,
including nonzero tool exits; input or process-launch errors produce no report.
Report-write failures terminate unsuccessfully with a Python diagnostic.
No tool version probe is run. Record toolchain and workload separately when
comparing measurements.

CI and release jobs save this report as the `naming-timings` artifact for 14 days.
An artifact-upload outage does not change the naming check result.

Run `python -m unittest tools.test_check_naming -v` to test selection,
ordering, failures, bounded concurrency, timings and temporary-file cleanup
with real Python subprocesses; no clang-tidy installation is needed.

<a id="tool-ci"></a>

## CI selection and final gate

[Source: checks.py](../tools/ci/checks.py). These commands are used by
[the PR workflow](../.github/workflows/ci.yml). They do not launch builds,
fetch refs, contact GitHub or change repository files.

### Select checks

```powershell
python tools/ci/checks.py select --base origin/main --head HEAD
```

| Argument | Required/default | Meaning |
|---|---|---|
| `select` | Required subcommand | Select `docs` or `full` |
| `--base REF` | Required; no default | Base Git revision already available locally |
| `--head REF` | Required; no default | Head Git revision already available locally |
| `-h`, `--help` | Optional on the main parser and each subcommand | Print the corresponding usage |

Requires Git on `PATH` and a current directory inside the intended checkout.
The exact comparison is `git diff --name-only --no-renames -z <base>...<head> --`:
it compares the merge base with the head. The checkout must contain the refs
and history needed to find that merge base. CI checks out the full history
and supplies the PR base and head SHAs. The command reads the full Git diff;
it does not use a paginated list of changed files from GitHub. NUL-separated
paths preserve spaces, non-ASCII characters and newlines. Disabling rename
detection includes both the old and new paths; deletions also participate.

The result is `docs` only for a nonempty list containing exclusively:

- `README.md` and `ReleaseNotes.md`;
- `.md` files anywhere under `docs/`;
- `.svg`, `.png`, `.jpg`, `.jpeg`, `.gif` and `.webp` files under `docs/images/`;
- `docs/diagrams.css`, `docs/DoxygenLayout.xml` and `docs/mermaid.json`.

Path and extension matching is case-sensitive. Every other path, a mixed diff
or an empty diff selects `full`. A C++ example under `docs/` and a documentation
processing script both require `full`.

| Environment variable | Default | Effect |
|---|---|---|
| `GITHUB_OUTPUT` | Unset or empty | If nonempty, append `mode=docs` or `mode=full` plus a newline to this UTF-8 file |
| `GITHUB_STEP_SUMMARY` | Unset or empty | If nonempty, append a Markdown heading, selection reason and changed-path count to this UTF-8 file |

Standard output is one JSON object with `mode`, `reason` and `paths`. The files
above are opened in append mode, so repeated calls append repeated entries.
Without these variables, no output files are written. A valid selection exits
`0`, including `full`. Argument errors exit `2`; Git or file-access errors
terminate unsuccessfully with a Python diagnostic.

### Validate results

`gate` takes no options of its own. It requires `CI_NEEDS`, a JSON object in the
GitHub Actions `needs` format. Example of successful documentation-only results:

```powershell
$env:CI_NEEDS = '{"changes":{"result":"success","outputs":{"mode":"docs"}},"docs":{"result":"success"},"naming":{"result":"skipped"},"smoke":{"result":"skipped"}}'
try {
  python tools/ci/checks.py gate
} finally {
  Remove-Item Env:CI_NEEDS
}
```

Run this example in a shell without an existing `CI_NEEDS` value. The workflow
sets the variable for its gate step from `toJSON(needs)`.

| Field | Required value |
|---|---|
| `changes.result` | `success` |
| `changes.outputs.mode` | `docs` or `full` |
| `docs.result` | `success` in both modes |
| `naming.result`, `smoke.result` in `full` mode | Both `success` |
| `naming.result`, `smoke.result` in `docs` mode | Each `success` or `skipped` |

Missing results, failure, cancellation or a skipped required job reject the
gate. On success it prints `All selected checks succeeded` and exits `0`.
Missing `CI_NEEDS`, invalid JSON, invalid result structure or rejected jobs
terminate unsuccessfully with a Python diagnostic; argparse errors exit `2`.
It does not write reports or use `GITHUB_OUTPUT` or `GITHUB_STEP_SUMMARY`.

### Selector regression tests

[Test source: test_checks.py](../tools/ci/test_checks.py) has no project-specific
CLI options. Use unittest discovery as shown in the command table; standard
unittest options control verbosity and test selection. Requires Python and Git
on `PATH`. Tests call `select` and `gate` against fixtures, initialize temporary
repositories and set repository-local test identities there. They do not
configure the working checkout or push anything. Temporary directories are
removed on completion. Results are printed by unittest: `0` means success,
`1` means test failure or error; invalid CLI usage exits unsuccessfully.

## HTML link checker

[Source: check_links.py](../tools/docs/check_links.py). Run after the documentation
target finishes:

```powershell
python tools/docs/check_links.py build/documentation/docs/html
```

The only accepted argument is one positional path to an existing generated
HTML directory. There are no switches, defaults or dedicated environment
variables. `--help` / `-h` prints usage without scanning files. It recursively reads UTF-8 `*.html`
files and collects `href`, `src`, `id` and legacy anchor `name` attributes.
For local references it checks target existence and, for parsed HTML targets,
fragment existence after URL decoding. External URLs are skipped and never
requested. CSS references, JavaScript-generated links and the content of
non-HTML targets are outside this check. Relative paths are resolved from
the referring page and can refer outside the supplied directory.

It prints a JSON object with `html_files`, `local_links_checked` and `broken`.
Each broken entry contains the source page relative to the HTML root, original
link and `missing file` or `missing anchor`. It does not modify the HTML.
Exit `0` means no broken local links; `1` means broken links, an invalid
directory or no HTML files; `2` means invalid CLI syntax. Input/read errors also terminate
unsuccessfully with a Python diagnostic.

<a id="tool-documentation"></a>

## HTML finalization

The supported documentation build runs finalization automatically:

```powershell
cmake -S . -B build/documentation -DPORTALS_BUILD_DOCS=ON
cmake --build build/documentation --target portals_docs
python tools/docs/check_links.py build/documentation/docs/html
```

Configuration requires CMake 3.24+, the project's C/C++ toolchain, Doxygen 1.18+
and Mermaid CLI (`mmdc` or `mmdc.cmd` on `PATH`); CMake locates Python 3.10+.
`PORTALS_BUILD_DOCS` defaults to `OFF`. A successful target produces
`<build>/docs/html` and `<build>/docs/portals.tag`. It does not publish them.

<a id="tool-html-processor"></a>

### HTML processing and CMake highlighting

[Source: finalize_html.py](../tools/docs/finalize_html.py). For diagnosing a
generated copy, its complete CLI is:

```powershell
python tools/docs/finalize_html.py build/documentation/docs/html build/documentation/docs/portals.tag --language-sites ru=build/documentation/docs/portals-ru.tag
```

| Argument | Required/default | Meaning |
|---|---|---|
| `html` | Required positional path | Existing Doxygen HTML directory to modify |
| `tag_file` | Required positional path | Doxygen XML tag file from the same documentation build |
| `--language-sites CODE=TAG ...` | Optional; omitted for a single-site diagnostic | Finalize all configured language sites, using the default tag plus one tag per additional language |
| `-h`, `--help` | Optional | Print usage and exit |

Paths resolve from the current directory. There are no dedicated environment
variables. Keep the processor in its checkout: it finds repository sources
relative to its own file. This is an in-place processor for generated output;
use a disposable build directory when investigating its behavior.

It processes the main page and guide pages listed in the tag file, then:

- links inline class, struct and concept names when the tag file resolves them
  unambiguously, preserving existing links and code blocks;
- resolves supported relative Markdown links and copies linked repository
  sources into `html/source/<repository-path>` for offline use; source copies
  must resolve inside the checkout;
- initializes local search for recognized Doxygen layouts missing the configuration element; layouts that provide it keep their own initialization;
- sets root `lang` and `xml:lang` from the configured site language; the single-site diagnostic retains filename-based detection;
- colors recognized plain CMake fragments using Doxygen CSS classes;
- adds guide headings through level three to the navigation tree and rebuilds
  navigation-index chunks of up to 250 entries while retaining API entries.

CMake coloring recognizes selected command signatures and applies simple
line-based token rules for commands, `${variables}`, quoted strings and `#`
comments. It is not a complete CMake parser; bracket strings and bracket
comments are not handled as multiline constructs. Ambiguous short type names
remain unlinked. The processor does not check external URLs or validate all
resulting links; run the link checker afterwards.

It rewrites guide HTML, `navtreedata.js`, generated `navtreeindex*.js` and the
recognized shared `search/search.js`, and
copies referenced source files. Standard output is JSON with `guide_pages`
and `unambiguous_types`. Success exits `0`, CLI errors `2`; malformed XML,
missing navigation data or read/write failures terminate unsuccessfully with
a Python diagnostic. There is no rollback for a partially processed tree;
regenerate the documentation after a failure.

### Language build preparation

[Source: prepare_languages.py](../tools/docs/prepare_languages.py) runs during
CMake configuration:

```powershell
python tools/docs/prepare_languages.py docs/languages.json docs/DoxygenLayout.xml --output build/documentation/languages
```

The positional `docs/languages.json` selects language metadata;
`docs/DoxygenLayout.xml` supplies navigation per language; `--output`
names the generated configuration directory. All three paths are required and
resolve from the current directory. `--help` needs no inputs and does no work.
The helper writes `languages.cmake` and `layout-<code>.xml`, replacing existing
outputs. It does not invoke Doxygen or access the network. Success exits `0`,
invalid data or I/O exits `1`, invalid CLI syntax exits `2`.

Doxygen also invokes its internal filter mode with `manifest`,
`--filter-language CODE`, `--source-root ROOT` and `--filter-input FILE`. The
filter writes the source to stdout, converting links to another language into
HTML links for later resolution. It preserves source Markdown and code blocks.
This mode does not write configuration files. No dedicated environment variables
are used in either mode.

The internal [language_site.py](../tools/docs/language_site.py) is called by the
HTML finalizer after every configured language has been built. It reads the
manifest, tag files and HTML, rewrites language links, adds the page selector and
creates redirects for old translated URLs. Invalid inputs or I/O fail the build;
regenerate the site after failure. It has no separate CLI.

### Processor regression tests

[Test source: test_finalize_html.py](../tools/docs/test_finalize_html.py) uses
unittest with no project-specific options or environment variables. The
discovery command above checks portable source links, inline type linking,
ambiguous names, page-language metadata, guarded search initialization, CMake text preservation and navigation/idempotence against
temporary fixtures. It does not require Doxygen or Mermaid and does not
process the real documentation tree. A writable system temporary directory
is required; fixture directories are removed after the tests. Unittest prints
results; exit `0` means success, `1` means failure or error.

## Internal CMake helpers

These files implement configured build targets. Invoke the owning target
instead of treating their internal variables as standalone CLI options.
They have no project-specific environment-variable interface.

| Helper | Caller and inputs | Result and failure behavior |
|---|---|---|
| [FinalizeDocumentation.cmake](../cmake/FinalizeDocumentation.cmake) | `portals_docs` post-build command sets `HTML_DIR` to `<build>/docs/html`; no default | Includes diagram finalization, copies the linked source/license notices and fixes recognized Doxygen index/link defects in place. Rejects missing/non-directory `HTML_DIR` and unrecognized index structures with a fatal CMake error. Source paths are relative to the helper's checkout. |
| [FinalizeDiagrams.cmake](../cmake/FinalizeDiagrams.cmake) | Included by `FinalizeDocumentation.cmake`, inheriting `HTML_DIR` | Scans top-level HTML pages and replaces Doxygen Mermaid SVG objects with responsive images. Requires referenced SVGs to contain a recognized `viewBox` and accessibility `title`; missing dimensions/title fail the build. Rewrites changed HTML; SVG files remain available. |
| [TestProcesses.cmake](../cmake/TestProcesses.cmake) | Included during configuration only when `WIN32` and `PORTALS_BUILD_TESTS` are true. CMake calls `portals_configure_test_processes(directory)` with the current source directory; `directory` has no default | Recursively visits executable targets already defined in that directory and its subdirectories, adding the test process setup source. It changes build target sources, then those sources are compiled during the normal build. |

The Windows setup is [test-owned source](../tests/support/no_dialogs.cpp).
It suppresses interactive system/CRT error dialogs, sends fatal CRT reports
to standard error and terminates the affected test process. It is not linked
into the core library. Validate its behavior through the configured CTest
case after building a Windows test-enabled tree:

```powershell
ctest --test-dir build/windows-msvc-debug -C Debug -R '^portals\.test-processes$' --output-on-failure
```

This test has a 40-second CTest timeout. Its probe and CMake driver are
[test-owned implementation files](../tests/no_dialogs.cmake); the supported
entry point is CTest. Successful CMake helper execution returns success to its
caller; fatal CMake errors fail the owning configure/build command.
