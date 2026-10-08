# Getting started {#getting-started}

Portals provides a C++23 static library and four runnable terminal examples.
Start with the gallery, then use the dashboard or custom widget to explore the
same [Application runner](application.md). The [architecture](architecture.md)
explains ownership and data flow; [limitations](limitations.md) defines the
boundaries of this release.

## Build from source

Requirements are CMake 3.24+, Ninja, a C99 compiler for the private vendored
utf8proc dependency, and a C++23 compiler **and standard library** with
`<expected>` and `<format>`. No dependency download is needed. Use an x64
Developer PowerShell for Visual Studio with the C++ desktop workload on Windows:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

On Linux with GCC and a compatible standard library. The preset uses the
`g++` on PATH; select an installed supported compiler explicitly when that
default is older, for example `cmake --preset linux-gcc-debug
-DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14` for GCC 14:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug
```

Presets build Debug under `build/<preset>`. For a separate Release build:

```powershell
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure --no-tests=error
```

The same Release commands work in a Linux shell. Keep compiler, architecture,
standard library and runtime compatible with consumers; see [package](package.md).
`PORTALS_BUILD_EXAMPLES`, `PORTALS_BUILD_TESTS` and `PORTALS_BUILD_TOOLS`
default to ON for top-level builds and OFF for embedded builds.

## Try the examples

In a terminal with native input/output attached, run:

```powershell
./build/windows-msvc-debug/examples/gallery/portals_gallery
./build/windows-msvc-debug/examples/dashboard/portals_dashboard --rate 10000 --fps 30 --rows 1000000
./build/windows-msvc-debug/examples/custom_widget/portals_custom_widget
./build/windows-msvc-debug/examples/replay/portals_replay_example
```

On Linux substitute `build/linux-gcc-debug`. F10 or Ctrl+C exits. Gallery:
Tab changes focus, Enter commits/activates, F2 changes theme. Dashboard: arrows,
Home and End navigate the table, Enter commits the note, F2 changes theme.
Custom widget: Left/Right adjusts the meter. [Examples](examples.md) describes
synthetic/replay sources, delay, ASCII and ANSI16 options.

Each executable also accepts `--check`. These finite checks use an in-memory
terminal and logical clock, exit automatically, and print results after close.
They verify runner behavior without requiring a native terminal; they do not
establish physical display compatibility or native restoration.

## Your own main

The complete small button program on [Application](application.md) checks every
creation/binding/run result. Its `Application` is constructed first, and its
scene and scoped binding are destroyed first. Keep the binding alive during
`run`; a temporary binding would cancel the action before the user could click.

For a complete form, the interactive path in `examples/gallery/main.cpp` is:

```cpp
auto app = example::take(portals::Application::create());
gallery::Form form(*app);

return example::report(app->run(form));
```

This excerpt uses example helpers, not additional public APIs:
`example::take` throws on a failed `expected`; main catches construction
failures. `example::report` handles the run result after restoration. See
`examples/gallery/form.hpp` for the actual nodes, properties, models and scoped
bindings owned by the form. The shorter main removes recurring loop mechanics,
not lifetime or error handling.

Derive `ApplicationScene` for global commands and preparation. In
`prepare`, synchronize bindings, pin each model once, prepare its sibling
views at their final extents, then handle returned provider requests outside
rendering. A custom widget implements `measure/arrange/paint`; prepare its
owning presentation before paint. `examples/custom_widget/main.cpp` shows
an independent immutable model and this lifecycle in full.

## Diagnose an action

```text
portals_dashboard --rate 0 --trace trace.json
```

Tab to Inspect and press Enter. Inspect assigns the note its existing value.
After exit, the metadata export can distinguish `handler_returned`,
`value_unchanged` and `frame_not_requested`. Export occurs after restoration;
file failures are reported separately. See [diagnostics](diagnostics.md) for
capacity/loss reporting and why an absent record proves nothing. Diagnostic
metadata differs from the versioned semantic replay format in [testing](testing.md).

## Installed package and HTML help

Use `add_subdirectory` and `portals::core` when embedding the source, or
`find_package(Portals CONFIG REQUIRED)` for an installed package.
[Package](package.md) supplies install, relocation and external consumer commands.

With Doxygen installed:

```powershell
cmake -S . -B build/docs -G Ninja -DPORTALS_BUILD_DOCS=ON
cmake --build build/docs --target portals_docs
```

Open `build/docs/docs/html/index.html`. The docs-only finalizer repairs two
recognized Doxygen 1.18 member-index anchor defects and copies linked license
notices. It fails explicitly if that repair encounters an unknown index shape.
An optional Python 3 gate checks all local file and anchor links:
`python tools/docs/check_links.py build/docs/docs/html`. Generated help belongs to the build
directory and needs no private work files. A downloadable archive containing
executables, HTML help and an installed SDK is planned; this page documents
source builds and installation, not an already validated archive layout.

References: [CMake presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html),
[CMake build tool mode](https://cmake.org/cmake/help/latest/manual/cmake.1.html),
[Doxygen configuration](https://www.doxygen.nl/manual/config.html).
