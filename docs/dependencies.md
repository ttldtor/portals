# Dependencies and licenses {#dependencies}

[Русская версия](ru/dependencies.md)

Portals uses the [Boost Software License 1.0](../LICENSE.txt).
The following table lists the third-party code and data included in its sources.
The linked notices contain the full license texts.

| Component | Purpose and delivery | License and source record |
|---|---|---|
| utf8proc 2.12.0 | Unicode decoding, [grapheme](glossary.md#glossary-text-units) boundaries, character properties and widths. Its vendored C sources build as a private static dependency of `portals::core`; its archive is included in the installed SDK. | MIT/Expat for code and Unicode notices for data: [license](../vendor/utf8proc/LICENSE.md), [source record](../vendor/utf8proc/README.md) |
| Lyra 1.8.0 | Optional command-line adapter `<portals/ui_lyra.hpp>` through `portals::lyra`; headers, license and manifest are installed with the SDK. | [Boost Software License 1.0](../vendor/lyra/LICENSE.txt), [pinned source record](../vendor/lyra/manifest.json) |
| Boost.PFR, boost-1.92.0 snapshot | Optional aggregate-column reflection through `<portals/aggregate_columns.hpp>` and `portals::aggregate`. Vendored headers are included in the SDK. | [Boost Software License 1.0](../vendor/pfr/LICENSE_1_0.txt), [source record](../vendor/pfr/README.md) |
| Textual palette values | Nord, Gruvbox, Tokyo Night and Catppuccin Latte colors adapted into Portals' built-in themes. No Textual Python runtime is required. | [MIT notice](../vendor/textual/LICENSE), [attribution and source record](../vendor/textual/README.md) |
| doctest 2.4.11 | Vendored single-header framework used by library tests; not included in the installed SDK or public headers. | [MIT license](../vendor/doctest/LICENSE.txt), [source record](../vendor/doctest/README.md) |
| Unicode 18.0.0 grapheme test data | Conformance fixtures used by Unicode tests. | [Unicode License V3](../tests/fixtures/unicode/Unicode-LICENSE.txt), [file hashes and origins](../tests/fixtures/unicode/manifest.json) |

## Build requirements

Building Portals from sources requires CMake 3.24+ and a toolchain supporting
C99 and C++23, with `<expected>` and `<format>` in the C++ standard library.
The guide's commands use Ninja. FetchContent's Git download requires Git.
All dependencies in the table are already stored in the Portals source tree;
building those sources does not download another dependency.

Your application's CMake project can specify `LANGUAGES CXX`. Portals enables
C in its own subdirectory to compile utf8proc as C99. Both language compilers
must be available in the selected toolchain. An installed SDK already contains
the archives, so a C++ application linking it does not need to compile C sources.

`portals::core` supplies the public include paths, the C++23 requirement and
the static link dependencies. On UNIX it also links the system Threads target.
See [package setup](package.md) for compiler, architecture and runtime compatibility.
The source records above identify the pinned versions and revisions.
utf8proc, Boost.PFR, Lyra, doctest and Unicode test data also have file manifests.
CMake verifies their file hashes when the corresponding build components are configured.

## Documentation tools

HTML styling uses [Doxygen Awesome 2.5.0](https://github.com/jothepro/doxygen-awesome-css).
The pinned CSS is stored with the source tree under the [MIT license](../vendor/doxygen-awesome/LICENSE).
Theme assets and the license are included in generated HTML for offline use.
The color-scheme button cycles through System, Light and Dark; its tooltip
names the current and next scheme. The choice is saved in browser storage when
available. Compact EN/RU links switch languages; tooltips show their full names. Navigation synchronization is switched at the
bottom of the sidebar.

Generating the HTML reference requires Doxygen 1.18 or newer and Mermaid CLI.
Node.js 24 LTS and Mermaid CLI 12 are the verified diagram toolchain.
These tools are not required to build an ordinary Portals application or read
the generated HTML. Installation instructions are in
[building Portals and its examples](getting-started.md).

The installed SDK carries license notices under `share/Portals` with the
default installation layout. The HTML reference includes the license texts
linked on this page for offline reading. See [package contents](package.md)
for installation and distribution layout.
