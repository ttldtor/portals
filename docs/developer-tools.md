# Developer tools {#developer-tools}

[Русская версия](ru/developer-tools.md)

Choose a command by the result you need. Run the commands from the repository
root unless a section says otherwise. The following pages describe the tools in
this source revision; the installed SDK and the release HTML may contain an older set.

| Task | Entry point | Reference |
| --- | --- | --- |
| Build the library and run tests | CMake and CTest | [Build and testing commands](developer-tools-build.md) |
| Check names | `python tools/check_naming.py` | [Naming](developer-tools-build.md#tool-naming) |
| Select and validate CI checks | `python tools/ci/checks.py` | [CI selection](developer-tools-build.md#tool-ci) |
| Build and check HTML help | `portals_docs`, then `check_links.py` | [Documentation tools](developer-tools-build.md#tool-documentation) |
| Measure source changes | `portals_compactness` | [Measurements](developer-tools-measure.md#tool-compactness) |
| Measure runtime costs | Five `portals_*_benchmark` executables | [Benchmarks](developer-tools-measure.md#tool-benchmarks) |
| Save and compare measurements | `tools/quality/performance.cmake` | [Performance receipts](developer-tools-measure.md#tool-performance) |
| Collect Clang-tidy observations | `tools/quality/readability.cmake` | [Readability](developer-tools-measure.md#tool-readability) |
| Inspect terminal behavior | `portals_terminal_probe` | [Terminal probe](developer-tools-measure.md#tool-probe) |
| Check native terminal cleanup | `native_windows.ps1` or `native_xterm.sh` | [Native checks](developer-tools-measure.md#tool-native) |
| Build a runnable bundle | `tools/package/bundle.ps1` or `bundle.cmake` | [Packaging](developer-tools-release.md) |
| Run an example from a bundle | Packaged `run.ps1` / `run.sh` | [Bundle launchers](developer-tools-release.md) |
| Prepare a release PR | `tools/release/start.ps1` or `start.py` | [Release preparation](developer-tools-release.md) |
| Assemble and publish release assets | Release workflow | [CI release helpers](developer-tools-release.md) |

## Reading command examples

PowerShell examples use paths relative to the checkout. Quote a complete
`-DNAME=value` argument when its value contains spaces or semicolons. CMake
script arguments precede `-P script.cmake`. With a multi-configuration generator,
add `--config Release` to builds and use the executable in its `Release` directory.
Measurement examples use Ninja Release and a dedicated `build/tools` directory.

A CLI's `--help` describes accepted arguments when implemented. Several benchmark
programs and shell launchers have no help mode; use their parameter tables here.
A failed check returns a nonzero status. Exact exit values are listed where the
implementation defines them; CMake and PowerShell failures are described as
nonzero instead of promising a portable numeric value.

## Internal helpers and tests

`tools/` contains both direct commands and support code. Shared CMake modules,
HTML finalizers and release publisher internals are identified in their reference
pages. Use the documented target or workflow that supplies their inputs.

`tests/*.cmake`, `tests/release_*.py` and the consumer-test scripts are test
implementations. Run them through CTest or their documented unittest command.
`tools/ci/test_checks.py` and `tools/docs/test_finalize_html.py` test the corresponding
helpers. They are included in the build/CI reference, not extra production commands.

See [testing](testing.md) for replay fixtures, fault injection and native supervisor
contracts, and [fuzzing](../fuzz/README.md) for sanitizer-oriented test executables.

## Local workspace scripts

The ignored `.work/` directory is private working material. It is absent from
source archives and SDKs. Files below `logs/`, `tmp/`, `artifacts/`, `experiments/`
and `worktrees/` can contain generated drivers, copied tools and dependencies.
A count of these files does not describe the maintained tool set.

Do not use a historical log script as a project entry point: it may contain an
absolute checkout path or assumptions about one experiment. Keep its original
receipt with the task that ran it. A reusable local helper belongs in the private
workspace index with its owner, requirements, inputs, output and measurement limits.
If the project starts depending on it, move it into maintained source with tests
and a public reference before adding it to CI. Workspace cleanup must preserve
unpublished changes and the evidence needed by active tasks.

## Maintenance

When changing a maintained tool, update its parameter table, command examples and
EN/RU pages together. Document defaults, units, required files, output locations,
exit behavior and any file, terminal or remote changes. Link new entry points from
this catalog. Keep test traces and run-specific evidence outside these guides.
