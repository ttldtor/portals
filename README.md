# Portals

Portals is a C++23 retained terminal UI library under development. The static core
includes an [Application runner](docs/application.md), checked widget ownership,
typed properties and immutable models, layouts, controls and LineEdit, virtual
lists/tables, Canvas/charts, styles/themes, bounded terminal output and causal
diagnostics.

Start with the [documentation guide and reading order](docs/index.md), then
[build and run your first example](docs/getting-started.md). See
[architecture](docs/architecture.md) for ownership, event flow and customization,
[limitations](docs/limitations.md) for platform/lifecycle/output boundaries, and
[installation](docs/package.md) for the relocatable CMake package.

## Build and test

Requirements: CMake 3.24+, Ninja and a compiler/standard library supporting C++23, including `<expected>` and `<format>` (MSVC or GCC).
The private vendored utf8proc dependency supplies Unicode 18.0.0 data; no automatic downloads or system Unicode library are required. A C99 compiler is needed alongside C++23.

On Windows, use an **x64 Developer PowerShell for Visual Studio** with the C++
desktop workload installed and CMake/Ninja on PATH:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

On Linux, with GCC, CMake and Ninja installed:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug
```

Builds live under `build/<preset>`. The presets select Debug; Release can be
checked separately, for example in the same Windows developer shell:

```powershell
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure --no-tests=error
```

`PORTALS_BUILD_TESTS` and `PORTALS_BUILD_EXAMPLES` default to ON for a top-level
build and OFF when embedded with `add_subdirectory`. Set either to OFF with
`-DPORTALS_BUILD_TESTS=OFF` / `-DPORTALS_BUILD_EXAMPLES=OFF`. Compiler warnings
are private to Portals targets; C++23 is a public target requirement.
Windows builds with tests enabled configure their executables to report fatal
CRT assertions and native exceptions without interactive debug dialogs. Failures
still return a nonzero exit code and diagnostics go to stderr for CTest to capture.
`portals.test-processes` checks standard assertions, abort, native exceptions, and
narrow/wide Debug CRT reports in separate processes. This configuration applies
only to these processes; it does not change Windows or Visual Studio settings.

## Terminal diagnostic probe

The library's move-only `TerminalSession` owns Windows/POSIX terminal modes and bounded
input/output, reports resize on its owner, and restores modes on close or stack
unwinding. See [the terminal session contract](docs/terminal.md) for ownership,
deadlines, errors and platform limitations.

Top-level builds include `portals_terminal_probe`. It reports native OS and
terminal facts, environment hints, modes, dimensions and bounded DA/CPR query
observations. To inspect without queries or mode changes:

```powershell
./build/windows-msvc-debug/portals_terminal_probe --no-query
```

In a terminal with stdin and stdout attached, use
`portals_terminal_probe --timeout-ms 250 --max-bytes 4096`.
Redirected handles automatically skip queries. `--headless` aliases `--no-query`;
`--help` lists all options. Set `PORTALS_BUILD_TOOLS=OFF` to omit the executable.
See [the probe contract and limitations](docs/terminal-probe.md) before interpreting
reply candidates or running interactive checks. No terminal identity or Unicode
width policy is authenticated by this diagnostic tool.

## Consume the library

Use `add_subdirectory(path/to/portals)` and link your target to `portals::core`.
The target supplies its public include path and language standard.
For an installed package, use `find_package(Portals CONFIG REQUIRED)` and the
same target. See [installation and relocation](docs/package.md) for commands,
toolchain/runtime compatibility and the independent consumer test.

The example is also a separate CMake project:

```powershell
cmake -S examples/consumer -B build/consumer -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/consumer
./build/consumer/portals_consumer
```

Expected output: `Portals 0.1.0`. The same commands work in a Linux shell.
The consumer includes only `<portals/version.hpp>` and links the core target.
`portals::version()` returns the linked library's version by value, without
allocation, I/O, errors or exceptions. Calls may run on any thread concurrently;
returned values have independent lifetime and cannot be invalidated. The version
does not promise API or ABI compatibility.

## API reference and IDEs

Doxygen 1.18 or newer is required only when enabling documentation; 1.18 is
the verified minimum for the complete reference. Ordinary library builds do not
require Doxygen:

```powershell
cmake -S . -B build/docs -G Ninja -DPORTALS_BUILD_DOCS=ON
cmake --build build/docs --target portals_docs
```

Open `build/docs/docs/html/index.html`. Public declarations include Doxygen contracts
for parameters, results, ownership, threading and errors. Generated help includes
the application, architecture, examples, installation and limitations pages.

In CLion, open the root CMake project and select the corresponding preset and
an installed toolchain. The Windows preset requires an MSVC environment. Local
adjustments belong in `CMakeUserPresets.json`; no IDE settings are supplied.

Reference: [CMake presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html),
[CMake compile features](https://cmake.org/cmake/help/latest/manual/cmake-compile-features.7.html),
[Doxygen integration](https://cmake.org/cmake/help/latest/module/FindDoxygen.html).

For the full topic map and suggested reading order, use the
[documentation guide](docs/index.md).

## License

Portals code: Boost Software License 1.0; see [LICENSE.txt](LICENSE.txt).
Private utf8proc code and Unicode data notices: [dependency provenance](vendor/utf8proc/README.md).
Unicode test data retain their [full license](tests/fixtures/unicode/Unicode-LICENSE.txt).

Adapted theme colors: [Textual attribution](vendor/textual/README.md).
