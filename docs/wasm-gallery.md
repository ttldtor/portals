# Browser Gallery and Level {#wasm-gallery}

[Русская версия](ru/wasm-gallery.md) · [Examples](examples.md) · [Developer tools](developer-tools.md)

This opt-in experimental sample runs Portals through WebAssembly and xterm.js.
Gallery provides an editor, checkbox, themes and a finite animation; Level accepts
device orientation from Telegram and draws a calibrated bubble level. Both scenes
use the existing fluent API. Native examples keep their synchronous runner.

WASM is optional: `PORTALS_BUILD_WASM_GALLERY` defaults to `OFF`. Existing native
presets and package consumers do not require Emscripten, Node.js, npm or xterm.js.
The commands below apply only when building the browser sample.

The source in this checkout is authoritative for this build. The separately hosted
POC is a historical deployment based on v0.3.0; it does not automatically track this
branch. A successful browser simulation does not qualify physical Telegram devices.

## Build the runtime

Use CMake, Ninja and Emscripten **6.0.12**, with its environment activated and
`EMSDK` pointing to the SDK root. Node.js **24** is used for the host tools.
From the repository root:

```powershell
cmake --preset wasm-debug
cmake --build --preset wasm-debug
cmake --build build/wasm-debug --target portals_wasm_sdk_smoke
node build/wasm-debug/examples/wasm_gallery/portals_wasm_sdk_smoke.js
```

Use `wasm-release` in place of `wasm-debug` for Release. The sample is controlled by
`PORTALS_BUILD_WASM_GALLERY`; it is off in normal native builds. These presets use
C++23 and native Wasm exceptions, with no Asyncify, JSPI or pthreads. They do not
build the native terminal examples or native test executables.

Build the static host beside the runtime:

```powershell
Push-Location examples/wasm_gallery/web
npm ci
npm run build -- ../../../build/wasm-debug/wasm-gallery
npm run build -- --help
Pop-Location
```

The output directory contains the Emscripten loader and WASM, bundled host JS/CSS,
HTML, source maps, license notices and artifact hashes in `provenance.json`.
The host builder requires the runtime files to exist and preserves them. It adds
content-based cache keys so a loader and its WASM use the same runtime version.
Serve this directory over HTTP for a local preview or HTTPS for a Mini App;
opening the HTML directly with `file:` is not a supported loading path.

For a local preview, Python's standard HTTP server is one option; Python is needed
only for this preview command:

```powershell
python -m http.server 8000 --bind 127.0.0.1 --directory build/wasm-debug/wasm-gallery
```

Open `http://127.0.0.1:8000/` and stop the server with Ctrl+C when finished. A static
HTTPS host can serve the same output directory; this sample needs no application
server. The WASM CI workflow builds and tests artifacts without publishing them.

## Use the scenes

Select **Gallery** or **Level** above the terminal. A switch cancels the old scene
and creates a fresh one, retaining the theme. **Quit**, F10 or Ctrl+C stops the
current scene; the selector remains available. Theme and F2 cycle the presets.
Tab and Shift+Tab traverse the terminal controls while focus is inside xterm.
Browsers may reserve function keys.

Gallery's **Run something** disables its button for one forward/backward pass,
changing the highlighted letter every 300 ms. Input and other actions remain
available. The minimum fitting targets are 22 columns/11 rows for Gallery and
25 columns/19 rows for Level. Font size has a 10px floor; smaller viewports can clip.

Mouse reporting uses SGR cell coordinates. A primary touch/pen tap up to 500 ms,
within 10 CSS pixels and the same terminal cell, uses the same input path. Swipes,
long presses and multiple fingers do not activate a control. Level sets the terminal
input read-only; Gallery restores text entry. Mobile keyboard behavior still needs
physical-device checks.

Level uses Telegram's DeviceOrientation API on clients supporting Mini Apps 8.0+.
It requests 50 ms updates, compensates quarter-turn screen rotation, applies a
low-pass filter and a 0.7° dead zone, and saturates the bubble at ±30°.
**Zero** takes the current pose as reference. Without a valid sample Zero is disabled;
an unavailable API is shown explicitly. There is no browser sensor fallback.
This sample is not a calibrated measurement instrument.

## Runtime and ownership

`Application::start()` and `Application::step()` expose incremental execution over
`ApplicationReadyTerminal`, whose input/output operations do not wait. Managed and
fluent scenes expose the corresponding lifecycle. The synchronous `run()` runner
drives the same core through the existing deadline transport.

The host calls one bounded step from `requestAnimationFrame`; it does not run a
blocking loop on the browser thread. The scene, its captured state and ready
transport must outlive all active steps. The sample destroys the scene before its
transport, cancels the Gallery timer on stop, and copies accepted output into
JS-owned storage before C++ teardown. Do not destroy the runtime from inside its
host output callback.

Scene generations reject sensor callbacks retained by a previous scene. Hidden or
inactive pages pause stepping and release sensor acquisition. The adapter permits
one outstanding Telegram start request and serializes stop acknowledgements.
A replacement waits until the previous stop callback and event dispatch finish.
If a start or stop never settles, a subsequent scene remains waiting.
Native global sensor events carry no request ID, so arbitrary
delayed native events across restarts cannot be attributed perfectly. Page hide
and fatal errors dispose host listeners and the runtime.

The sample transport and exports live in
[browser_terminal.hpp](../examples/wasm_gallery/browser_terminal.hpp) and
[main.cpp](../examples/wasm_gallery/main.cpp). The scene implementations are
[demo_scene.hpp](../examples/wasm_gallery/demo_scene.hpp) and
[level_scene.hpp](../examples/wasm_gallery/level_scene.hpp).

## Verify a build

After building the runtime and host:

```powershell
Push-Location examples/wasm_gallery/web
npm test
npx playwright install chromium
$env:PORTALS_WASM_DIR = (Resolve-Path ../../../build/wasm-debug/wasm-gallery).Path
npm run test:browser
Pop-Location
```

`PORTALS_WASM_DIR` selects the built output; `PORTALS_WASM_PORT` defaults to 41813.
Playwright owns and stops its server, serving beneath a project URL prefix to check
relative asset paths. Unit tests cover output backpressure, touch and sensor leases;
browser tests use the real WASM with simulated SDK input. Physical Desktop,
Android/iOS, IME and sensor qualification remain separate checks. The broader
runtime integration also requires the native matrix and package-consumer tests
listed in [development](development.md).
