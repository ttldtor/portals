# Build and documentation tools {#developer-tools-build}

[Русская версия](ru/developer-tools-build.md)

This reference covers naming checks, CI check selection and generated HTML.
Run the PowerShell commands from the repository root. Use Python 3.10 or newer;
these Python tools use the standard library. Paths below are examples and may
be replaced with your build directories.

## Choose a command

| Task | Command or target | Changes |
|---|---|---|
| Check C++ names | `python tools/check_naming.py <build>` | Runs clang-tidy; does not request fixes |
| Determine required PR checks | `python tools/ci/checks.py select --base <base> --head <head>` | Reads Git; optionally appends CI output files |
| Validate completed CI jobs | `python tools/ci/checks.py gate` | Reads `CI_NEEDS` |
| Build the complete HTML documentation | `cmake --build <build> --target portals_docs` | Generates and finalizes the HTML tree |
| Check generated links | `python tools/docs/check_links.py <html>` | Reads HTML and referenced local paths |
| Test the CI selector | `python -m unittest discover -s tools/ci -p test_checks.py -v` | Creates temporary Git repositories |
| Test HTML processing | `python -m unittest discover -s tools/docs -p test_finalize_html.py -v` | Creates temporary fixture files |

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
| `-h`, `--help` | Optional | Print usage and exit |

The script selects unique translation units under `src`, `tests`, `tools`,
`examples`, `experiments` and `fuzz` in this checkout. Files outside the checkout
are skipped. The deliberately invalid `static_scene`, `static_styles` and
`ct_pss` fixtures named `tests/fixtures/<fixture>/invalid.cpp` are also skipped.
Only translation units present in the database are checked.

For each file, it invokes clang-tidy with `-p <build>`, `--quiet` and the
repository `.clang-tidy` as `--config-file`, using the repository root as its
working directory. There is no `--fix` option. Diagnostics go to the console;
the final line reports checked and failed translation units. No report file
is created. The script reads no dedicated environment variables; executable
lookup and compiler discovery still depend on the process environment.

Exit codes: `0` if every invocation succeeds, `1` if any clang-tidy invocation
fails, `2` for invalid arguments or a database with no selected sources.
Unreadable or malformed JSON and process-launch errors terminate unsuccessfully
with a Python diagnostic. The script continues after a clang-tidy nonzero exit
to check the remaining files.

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
python tools/docs/finalize_html.py build/documentation/docs/html build/documentation/docs/portals.tag
```

| Argument | Required/default | Meaning |
|---|---|---|
| `html` | Required positional path | Existing Doxygen HTML directory to modify |
| `tag_file` | Required positional path | Doxygen XML tag file from the same documentation build |
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
- colors recognized plain CMake fragments using Doxygen CSS classes;
- adds guide headings through level three to the navigation tree and rebuilds
  navigation-index chunks of up to 250 entries while retaining API entries.

CMake coloring recognizes selected command signatures and applies simple
line-based token rules for commands, `${variables}`, quoted strings and `#`
comments. It is not a complete CMake parser; bracket strings and bracket
comments are not handled as multiline constructs. Ambiguous short type names
remain unlinked. The processor does not check external URLs or validate all
resulting links; run the link checker afterwards.

It rewrites guide HTML, `navtreedata.js` and generated `navtreeindex*.js`, and
copies referenced source files. Standard output is JSON with `guide_pages`
and `unambiguous_types`. Success exits `0`, CLI errors `2`; malformed XML,
missing navigation data or read/write failures terminate unsuccessfully with
a Python diagnostic. There is no rollback for a partially processed tree;
regenerate the documentation after a failure.

### Processor regression tests

[Test source: test_finalize_html.py](../tools/docs/test_finalize_html.py) uses
unittest with no project-specific options or environment variables. The
discovery command above checks portable source links, inline type linking,
ambiguous names, CMake text preservation and navigation/idempotence against
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
