# Lyra snapshot

This directory contains the unchanged `include/` tree and `LICENSE.txt` from
[bfgroup/Lyra](https://github.com/bfgroup/Lyra), stable tag
[`1.8.0`](https://github.com/bfgroup/Lyra/releases/tag/1.8.0), commit
`4186a07f52392aaac31726c64c574d3496cdc13a`.

Lyra is distributed under the Boost Software License 1.0 (BSL-1.0).
`manifest.json` records the official source archive URL, SHA-256, and each
vendored file's size and SHA-256. Configure verifies these files offline.
No Boost superproject, submodules, system installation or network access is
required to build or consume Portals.

The optional `portals::lyra` target supplies the headers for
`<portals/ui_lyra.hpp>` and native `<lyra/lyra.hpp>` use. `portals::core` does
not add Lyra's include path or include Lyra from its public headers.
