# Building Portals and its examples {#getting-started}

[Русская версия](ru/getting-started.md)

Portals provides a C++23 static library and five runnable terminal examples.
Start with the gallery, then use the dashboard or custom widget to explore the
same [Application runner](application.md). The [architecture](architecture.md)
explains ownership and data flow; [limitations](limitations.md) defines the
boundaries of this release.

The [documentation guide](index.md) gives the reading order and separates
application guides from core contracts and contributor material.

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

On Linux, use GCC with a compatible standard library. The preset uses the
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
./build/windows-msvc-debug/examples/editor_history/portals_editor_history
./build/windows-msvc-debug/examples/click_handlers/portals_click_handlers
```

On Linux substitute `build/linux-gcc-debug`. F10 or Ctrl+C exits. Gallery:
Tab changes focus, Enter commits/activates, F2 changes theme. Dashboard: arrows,
Home and End navigate the table; valid note edits apply immediately, and F2 changes theme.
Custom widget: Left/Right adjusts the meter. [Examples](examples.md) describes
synthetic/replay sources, delay, ASCII and ANSI16 options.

The gallery also accepts `--theme NAME` (or `-t NAME`), `--ansi16` and `--help`.
Its native Lyra schema runs before any terminal is opened; help returns zero,
and invalid options return two. The public program has no `--check` mode.

Finite checks live in separate test harnesses and the installed-package consumer.
They use an in-memory terminal and logical clock, exit automatically, and verify
runner behavior without establishing physical display compatibility.

## Your own main

Start with [the complete click-handler program](../examples/click_handlers/main.cpp)
and [declarative composition](declarative-ui.md). Describe controls, attach handlers,
and finish the scene with `ui::runApp()`. The owning scene retains the application,
models, bindings and standard presentation adapters. Ref copies observe controls.

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> message;
  ui::Ref<Button> add;
  auto scene = ui::scene(ui::vbox(ui::lineEdit(message, ""), ui::button(add, "Add Hello")));

  return scene
    | ui::onClick(add, [message] {
           message.text() += "Hello!";
         })
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
```

The final stage returns zero on success or a failure code after cleanup and default
error reporting. There is no application-written event loop or stop source here.
For a larger complete form, read [the gallery function](../examples/gallery/fluent_form.hpp)
and its [entry point](../examples/gallery/main.cpp). The form returns a lazy
scene from `gallery::form(ui::opts(options))`; a native Lyra declaration fills
the caller-owned options through the final `ui::runApp(argc, argv, parser)` stage.
The options must outlive the synchronous call. See [CLI startup](cli.md) for
`ui::lyra::cli`, typed errors, help/version and read-only process snapshots.
[Managed controls](managed-controls.md) explains custom presentation adapters.
For explicit low-level ownership, [Application](application.md) shows a small
button program with an `ApplicationScene` and a retained scoped binding.
Derive `ApplicationScene` when managing commands and preparation yourself. In
`prepare`, synchronize bindings, pin each model once, prepare its sibling
views at their final extents, then handle returned provider requests outside
rendering. A custom widget implements `measure/arrange/paint`; prepare its
owning presentation before paint. `examples/custom_widget/meter.hpp` shows
an independent immutable model and a managed preparation adapter.

## Diagnose an action

```text
portals_dashboard_check --rate 0 --trace trace.json
```

This command runs the test-owned diagnostic harness, built with `PORTALS_BUILD_TESTS=ON`.
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
For the optional Lyra adapter, link `portals::lyra`; its headers, BSL-1.0 license
and pinned manifest are included in the offline SDK. Core-only applications
continue to link `portals::core`.

For HTML documentation, install Doxygen 1.18 or newer and Mermaid CLI. The
verified diagram toolchain is Node.js 24 LTS with Mermaid CLI 12. In PowerShell:

```powershell
npm.cmd install --global @mermaid-js/mermaid-cli
mmdc.cmd --version
```

Mermaid CLI uses a headless browser supplied by Puppeteer. Its first installation
normally downloads that browser; subsequent documentation builds render diagrams
locally. Ordinary library builds do not need Node.js, Mermaid or Doxygen.

With `doxygen` and `mmdc` available on PATH:

```powershell
cmake -S . -B build/docs -G Ninja -DPORTALS_BUILD_DOCS=ON
cmake --build build/docs --target portals_docs
```

If Mermaid is installed outside PATH, pass its executable explicitly with
`-DPORTALS_MERMAID_EXECUTABLE=C:/path/to/mmdc.cmd` (or the `mmdc` executable on
POSIX). CMake uses its directory for Doxygen's native Mermaid integration.

Open `build/docs/docs/html/index.html`. Start with the reading guide; use the
sidebar's API reference when you need a specific type or function. The HTML
directory includes example source listings and license notices for offline
reading. Mermaid blocks are rendered to SVG at build time; reading diagrams
requires neither internet access nor an installed generator. Generated help
needs no private work files.

When changing documentation, check its local file and anchor links with
`python tools/docs/check_links.py build/docs/docs/html` (Python 3 is optional
for ordinary builds). This page covers source builds and installation;
the [package guide](package.md) describes distribution and consumer checks.

References: [CMake presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html),
[CMake build tool mode](https://cmake.org/cmake/help/latest/manual/cmake.1.html),
[Doxygen configuration](https://www.doxygen.nl/manual/config.html).
