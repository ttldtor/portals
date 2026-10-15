# Packaging and release tools {#developer-tools-release}

[Русская версия](ru/developer-tools-release.md) · [Developer tools](developer-tools.md) · [Release procedure](releasing.md)

This reference describes the maintained entry points in `tools/package` and
`tools/release`. Run repository commands from the checkout root. Commands which
create branches, upload assets or publish a release require release authorization.

`bundle.cmake` and `tools/release/package.cmake` accept `-DHELP=ON`
(default `OFF`): print embedded help and return `0` before path validation or
file writes. For PowerShell use `Get-Help <script-path> -Full`; Python supports
`--help` and `-h`.

## Choose an entry point

| Task | Entry point | Result / effects |
| --- | --- | --- |
| Build a runnable archive from a commit | `tools/package/bundle.ps1`, or `cmake -P tools/package/bundle.cmake` | Local Release build, SDK, five examples, HTML, checks and archive |
| Run an example from an extracted archive | Archive's `run.ps1` or `run.sh` | Starts the selected terminal application |
| Prepare a release PR | `tools/release/start.ps1` or `start.py` | Updates version/notes in a temporary worktree; commits, pushes, opens PR |
| Prepare release metadata | `tools/release/metadata.py prepare` | Local JSON and release notes; no network |
| Assemble release artifacts | `tools/release/package.cmake` | CI helper using verified builders' outputs and receipts |
| Publish verified artifacts | `tools/release/publish.py` | CI publisher; creates/resumes a GitHub draft, uploads, verifies and publishes |
| Shared CMake functions | `tools/package/common.cmake` | Internal include; no standalone CLI |

Reserve the release task ID before preparing a release. Preparation uses component
`BUILD`: for task `PRTLS-143` and tag `v0.3.1`, the branch is
`task/PRTLS-143-BUILD-prepare-release-v0.3.1` and the commit/PR title is
`[PRTLS-143][BUILD] Prepare release v0.3.1`. Supply the actual reserved task ID.
The CLI validates its syntax; it does not check the shared task registry.

## Build a runnable bundle

Sources: [CMake implementation](../tools/package/bundle.cmake),
[PowerShell wrapper](../tools/package/bundle.ps1).

Requirements: CMake 3.24+, Git, a native x64 Windows or Linux host and C++23/C
compilers. Windows requires MSVC for both languages and `dumpbin` in the compiler
environment; Linux requires `ldd`. The default generator requires Ninja.
The documentation target requires Doxygen 1.18+, Python 3.10+ and Mermaid CLI
(`mmdc`/`mmdc.cmd`, with its Node.js runtime) on `PATH`.
The selected commit must contain the package tooling.

Choose fresh, absolute work and output paths. The example writes outside the
checkout; change these paths for your machine:

```powershell
pwsh -NoProfile -File tools/package/bundle.ps1 `
  -WorkDir C:/artifacts/portals-build-001 `
  -OutputArchive C:/artifacts/portals-001.zip `
  -PythonExecutable python
```

`bundle.ps1` accepts relative paths and converts them to absolute paths before
calling CMake. Direct CMake calls require absolute paths already:

```powershell
cmake -DSOURCE_DIR=C:/src/portals -DSOURCE_REVISION=HEAD `
  -DWORK_DIR=C:/artifacts/portals-build-002 `
  -DOUTPUT_ARCHIVE=C:/artifacts/portals-002.zip `
  -DPYTHON_EXECUTABLE=python -P tools/package/bundle.cmake
```

On Linux use a `.tar.gz` output and the same CMake parameters. Quote each whole
`-DNAME=value` argument when its value contains spaces. Put all `-D` arguments
before `-P`.

### Bundle parameters

All CMake parameters use `-DNAME=value`. A dash in the wrapper column means
that the parameter is available only through the CMake entry point.

| CMake parameter | PowerShell parameter | Default | Meaning / validation |
| --- | --- | --- | --- |
| `SOURCE_DIR` | `-SourceDir` | Required directly; wrapper uses repository containing the script | Existing absolute Git checkout |
| `SOURCE_REVISION` | `-Revision` | `HEAD` | Revision which resolves to a commit; only its committed files are archived |
| `WORK_DIR` | `-WorkDir` | Required | New absolute path outside source; no existing file, directory or symlink; no `;` |
| `OUTPUT_ARCHIVE` | `-OutputArchive` | Required | New absolute path outside work directory; no existing file, directory or symlink; no `;`; Windows `.zip`, Linux `.tar.gz` |
| `GENERATOR` | `-Generator` | `Ninja` | CMake generator for the build and consumer checks |
| `PARALLEL` | `-Parallel` | `2` | Integer `>=1`; wrapper additionally limits it to 1–1024 |
| `MSVC_RUNTIME_LIBRARY` | `-Runtime` | `MultiThreaded` | Windows: `MultiThreaded` (`/MT`) or `MultiThreadedDLL` (`/MD`); Linux records `not-applicable` |
| `CXX_COMPILER` | `-CxxCompiler` | CMake discovery | Explicit C++ compiler |
| `C_COMPILER` | `-CCompiler` | CMake discovery | Explicit C compiler |
| `MAKE_PROGRAM` | — | CMake discovery | Override `CMAKE_MAKE_PROGRAM`, for example a Ninja path |
| `PYTHON_EXECUTABLE` | `-PythonExecutable` | Unset | Runs the HTML link checker and passes its executable to fresh-unpack verification; separate from documentation's Python discovery |
| — | `-CMake` | `cmake` | CMake executable used by the wrapper |

Without `PYTHON_EXECUTABLE`, the optional explicit HTML link check is recorded
as `linkStatus=not_run`. Release packaging requires `passed`, so supply it for
release bundles. This option does not remove Python's documentation-build requirement.

### Bundle output and failure recovery

The script exports the chosen commit with `git archive`; uncommitted and ignored
files are excluded. It configures Release with tests, examples and docs enabled,
tools/fuzzers disabled, then builds the five example executables and documentation
and installs the SDK. It checks dependency imports and rejects dynamic CRT imports
when `/MT` was requested.

The work directory retains `source.tar`, `source/`, `build/`, the
`portals-mvp-<platform>-x64-release/` payload, a candidate archive, fresh-unpack
checks and `bundle-receipt.txt` on success. The final archive contains:

- `bin/`: dashboard, gallery, custom widget, editor history and click handlers;
- `install/`: SDK; `examples/`: source and external-consumer project;
- `docs/html/`, `themes/`, `data/`, `licenses/`, launchers and `manifest.json`;
- `verify/`: the fresh-unpack CMake check, shared helpers and HTML link checker.

A fresh extraction and relocated SDK consumer must pass before the archive is
copied to the output filesystem and published locally with no overwrite. SHA-256
values record source, payload and archive bytes; they are integrity checks,
not signatures. Native interactive terminal acceptance is a separate check.

CMake returns 0 on success and a nonzero status on validation/build/check failure.
The wrapper throws on a failed CMake command; `pwsh -File` reports a nonzero exit.
Failures preserve staging for diagnosis. Use a new work/output path for a retry;
the script never clears an old staging directory.

## Run an extracted example

Sources: [PowerShell launcher](../tools/package/run.ps1),
[POSIX shell launcher](../tools/package/run.sh).
These scripts are copied beside the archive's `bin/`; run those copies, rather
than invoking the repository copies against a build directory.

```powershell
pwsh -NoProfile -File C:/artifacts/unpacked/portals-mvp-windows-x64-release/run.ps1 -Example gallery
```

```sh
./run.sh gallery
```

`run.ps1` accepts `-Example` (default `dashboard`) and remaining positional
`ExampleArguments`. `run.sh` takes the example as its first positional argument
(default `dashboard` when omitted), and forwards all remaining arguments.
Both accept `dashboard`, `gallery`, `custom_widget`, `editor_history` and
`click_handlers`, locate `bin/` relative to the script and return the application's
exit status. PowerShell rejects invalid selections and throws if the executable
is missing. The shell reports an unknown selection to stderr and exits 2;
execution failures follow the shell's exit conventions. Neither launcher changes
runtime settings or searches for a compiler. Binary compatibility requirements
are recorded in the bundle manifest and [package guide](package.md).

## Prepare a release pull request

Sources: [PowerShell](../tools/release/start.ps1), [Python](../tools/release/start.py).
Use either alternative. Windows PowerShell 5.1/PowerShell 7 supports the first;
Python 3.10+ supports the second. Both need Git, a clean tracked/untracked worktree,
a GitHub `origin` with one push URL identifying the same repository, network access
and, for a normal run, authenticated GitHub CLI (`gh`). Git commit identity and
push/PR permissions must be configured already. Ignored files in the starting
checkout are not included in the clean-worktree check.

Preview first; replace the example tag with the intended release:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/release/start.ps1 -Tag v0.3.1 -TaskId PRTLS-143 -DryRun
python tools/release/start.py v0.3.1 --task-id PRTLS-143 --dry-run
```

For an authorized preparation, omit `-DryRun` / `--dry-run` from one command.
The PowerShell execution policy flag applies only to that process.

| PowerShell | Python | Default / contract |
| --- | --- | --- |
| `-Tag` (also positional argument 0) | Positional `tag` | Required in preparation mode; `vX.Y.Z` or `vX.Y.Z-rcN`, no leading zeros, `N` positive |
| `-TaskId` | `--task-id` | Required `PRTLS-N` or `PRTLS-N.N`; each numeric part is positive with no leading zeros; component is `BUILD` |
| `-DryRun` | `--dry-run` | Off; prints JSON fields `tag`, `task_id`, `branch`, `commit_title`, `pr_title`, `base`, `main_commit`, `changed_files`, `dry_run=true` |
| — | `--dispatch-merged EVENT_JSON` | Actions-only mode; mutually exclusive with preparation arguments |

Dry run performs `git ls-remote`; it still needs remote access. It writes no
repository files/refs and does not fetch objects. The remote main commit must
already exist locally. It refuses if the preparation branch or release tag
already exists; the normal command validates/resumes existing state.

The normal command fetches main, prepares only `CMakeLists.txt` and
`ReleaseNotes.md` in a temporary worktree, commits, pushes and opens a PR, printing
the PR URL. It requires one first, nonempty `Unreleased` section. Stable releases
date that section and leave an empty generic `Unreleased`; RCs copy the working
entries into a dated RC section. The CMake version remains numeric. Existing
branches, tags and PRs are validated before reuse; conflicting or closed state
requires inspection. Commit and PR titles must match the task/release identity.
Another preparation branch reusing the task ID or tag is rejected. An old
`task/release-<tag>` branch for the requested tag requires explicit migration;
the script does not rename it. No force push or tag replacement is performed.

A clean temporary worktree is removed. Dirty or ignored recovery files cause it
to be retained, with its path printed to stderr. Child Git/gh processes receive
`GIT_OPTIONAL_LOCKS=0`. Both entry points report 0 on success, 1 for handled
preparation failures; Python argparse usage errors return 2. PowerShell parameter
binding errors fail before the script's handler.

Merging a matching same-repository preparation PR into main authorizes the
configured release automation. Python's `--dispatch-merged` mode consumes a trusted
merged-PR event, requires `GITHUB_REPOSITORY=owner/name`, validates the checked-out
merge commit/version/notes and ancestry in `origin/main`, creates an annotated
tag only if absent, and dispatches `release.yml` with publication enabled.
Legacy `task/release-*` branches are not accepted by the dispatcher and do not
automatically publish. It needs authenticated `gh` with contents/actions write access. This is an
internal Actions entry point; use the [release procedure](releasing.md) for recovery.

## Prepare metadata locally

Source: [metadata.py](../tools/release/metadata.py). Python 3.10+; no GitHub CLI
or network required.

```powershell
python tools/release/metadata.py prepare --tag v0.3.0 `
  --repository ttldtor/portals --source . --output-dir C:/artifacts/portals-metadata
```

Use a tag matching the source version and release-notes section.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `prepare` | Required subcommand | Generate metadata and notes |
| `--tag` | Required | Valid stable/RC tag; numeric base must match the single CMake project version |
| `--repository` | Required | `owner/name`, using letters, digits, `_`, `.`, `-`; used for links |
| `--source` | Required | Source directory containing `CMakeLists.txt` and `ReleaseNotes.md` |
| `--output-dir` | Required | Created recursively; `release.json` and `release-notes.md` are overwritten if present |
| `--github-output` | Unset | Append `tag`, `version`, `prerelease` to this existing-parent output file |

Duplicate selected notes sections, empty notes and stable `Unreleased` notes are
rejected. RCs may use their exact section or fall back to the base-version section.
Relative notes links become tag-specific GitHub links; links escaping the source
root are rejected. RC notes receive a testing notice. Exit: 0 success, 1 handled
metadata/file/encoding failure, 2 argparse usage error. This script does not read
`GITHUB_OUTPUT` automatically: pass its path explicitly if needed.

## Assemble release assets in CI

Source: [package.cmake](../tools/release/package.cmake). Internal CMake 3.24+/Git
helper called by [release.yml](../.github/workflows/release.yml). It consumes
already checked builds; it does not run the full Debug/Release suites itself.
The workflow supplies receipts after those checks. Do not fabricate receipts to
invoke packaging against an unchecked build.

| `-D` parameter | Required / default | Contract |
| --- | --- | --- |
| `SOURCE_DIR` | Required | Absolute existing source checkout; HEAD and tag must equal `SOURCE_REVISION` |
| `SOURCE_REVISION` | Required | Full 40-character lowercase hexadecimal commit |
| `RELEASE_TAG` | Required | Stable/RC tag described above |
| `PLATFORM` | Required | `windows` or `linux` |
| `PAYLOAD_DIR` | Required | Existing extracted bundle payload; manifest, hashes and sibling `bundle-receipt.txt` checked |
| `OUTPUT_DIR` | Required | Existing directory initially containing only `portals-<tag>-<platform>-x64-release.zip` (Windows) or `.tar.gz` (Linux) |
| `WORK_DIR` | Required | New absolute staging path, no `;`; separate from source, payload and output |
| `CONFIGURATION` | Required | Exactly `Release` |
| `PARALLEL` | Required | Integer `>=1`, used for Linux source-consumer build |
| `TOOLCHAIN` | Required | Identity matching both gate receipts |
| `DEBUG_GATE_RECEIPT`, `RELEASE_GATE_RECEIPT` | Required | Absolute regular, nonsymlink JSON files matching schema 1, tag, commit, platform, configuration, toolchain and `headless=passed` |
| `C_COMPILER`, `CXX_COMPILER`, `GENERATOR` | Required on Linux | Explicit offline source-consumer toolchain; unused on Windows |
| `PAGES_DIR` | Unset | Linux only: new absolute standalone directory for copied HTML and `.nojekyll` |

Source, payload and output directory paths must be absolute existing directories
with no `;` and no symlink as the final path component. Output must be outside
source and separate from payload. Staging and optional Pages directory must not
contain, or be contained by, the other paths; existing ancestors are resolved to
detect symlink escapes. Payload inventory rejects symlinks.

The helper verifies the archive against the payload manifest and receipt. Linux
adds source ZIP/tar.gz, builds/runs an offline extracted source consumer, adds an
HTML ZIP, and optionally copies Pages HTML. Both platforms write
`release-manifest-<platform>.json` with sizes, hashes, toolchain and gates.
The helpers write local artifacts only; Pages deployment happens later in Actions.
On error CMake returns nonzero and can leave partial output/staging. Recreate the
expected input directory for a retry; existing source/docs archive names can be
overwritten by the archive commands. Exit 0 means packaging checks passed;
interactive native acceptance remains a separate gate.

## Validate or publish the complete artifact set

Source: [publish.py](../tools/release/publish.py). Python 3.10+. Publication also
requires authenticated `gh` with release write/read access. The workflow supplies
`GH_TOKEN`; the script itself uses gh's authentication environment/configuration.
The local validation mode does not contact GitHub:

```powershell
$releaseCommit = (Get-Content C:/artifacts/release-candidate/release-manifest-windows.json -Raw | ConvertFrom-Json).commit

python tools/release/publish.py --directory C:/artifacts/release-candidate `
  --repository ttldtor/portals --tag v0.3.0 `
  --commit $releaseCommit --dry-run
```

The command reads the commit from the Windows build manifest. Both platform
manifests must identify that same commit.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `--directory` | Required | Existing directory with metadata, notes, Windows/Linux manifests and every listed artifact |
| `--repository` | Required | GitHub `owner/name`; validated when constructing the GitHub client in publication mode |
| `--tag` | Required | Stable/RC tag matching metadata and both build manifests |
| `--commit` | Required | Commit matching both manifests; publication additionally requires a full lowercase 40-hex SHA and matching remote tag |
| `--github-output` | Unset | After publication, append `latest` and `pages_deploy` booleans; ignored during dry run |
| `--dry-run` | Off | Local validation and inventory only; overwrites `release-manifest.json` and `SHA256SUMS` |

Required build gates are `debug_headless`, `release_headless`, `bundle_smoke` and
`html_links`, all `passed`. The artifact set must include both platform bundles,
source ZIP/tar.gz and HTML ZIP. Unlisted assets, missing or changed files, unsafe
names and symlinks are rejected. Metadata and notes are excluded from uploaded
assets; platform manifests, final manifest and checksum list are uploaded.

Publication verifies the remote tag, creates or resumes a draft, checks existing
metadata/assets, uploads missing files, downloads them to check hashes, then
publishes. Conflicting assets are never overwritten. An interrupted draft remains
available for a validated retry. RCs never become latest; a stable version becomes
latest only if numerically newer than the current latest. `pages_deploy=true`
means this tag is the resulting latest stable release; deployment is a later job.
The script writes no tags and does not merge PRs. Exit: 0 success; argparse usage
errors 2; other failures produce a Python error and nonzero exit (normally 1).

## Shared bundle helpers

Source: [common.cmake](../tools/package/common.cmake). Included by bundle creation,
fresh-unpack tests and release packaging. It has no user parameters, environment
contract or standalone invocation.

| Function | Purpose |
| --- | --- |
| `bundleStageConsumer(source, destination)` | Copy the external-consumer project, examples and test support |
| `bundlePublishArchive(pendingArchive, outputArchive, outputHash)` | Verified copy to the output filesystem and no-overwrite rename; return SHA-256 |
| `bundleRun(...)` | Execute a command, stop CMake on nonzero status |
| `bundleJsonQuote(output, value)` | Escape a string for generated JSON |
| `bundleRequireNewPath(variable)` | Normalize/check a new absolute path variable and return it to the caller |
| `bundleValidateRelativePath(path)` | Reject unsafe payload-relative paths |
| `bundlePathIsInside(parent, child, output)` | Resolve paths and return containment, case-insensitive on Windows |
| `bundleRejectSymlinks(root)` | Reject symlinks discovered in a payload tree |
| `bundleWriteHashes(root, output)` | Generate sorted file hashes excluding `manifest.json` |
| `bundleVerifyHashes(root)` | Check schema 1, complete file inventory and hashes |

None of these tools defines a general `--verbose`, cleanup or overwrite switch.
Use the caller's logs and preserved staging. Ordinary inherited tool settings
(`PATH`, compiler discovery, Git configuration and gh authentication) still apply.

Python entry points also support `-h` / `--help` for usage. PowerShell scripts
with `[CmdletBinding()]` accept standard common parameters; list them with
`Get-Help <script> -Full`.
