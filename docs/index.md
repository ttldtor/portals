# Portals: short library guide

[Русская версия](ru/index.md) · [Task guides](guides.md) · [Documentation contents](reference-map.md) · [Glossary](glossary.md)

Portals is a C++23 terminal UI library under development.
The declarative `ui` API is the primary way to build an interface.
This page is the short reading guide; the [documentation contents](reference-map.md)
contains the complete topic list and API reading paths.

These Markdown guides describe the current source branch. The [published HTML help](https://ttldtor.github.io/portals/) follows the latest stable release; PR checks also provide an HTML preview artifact.

Follow the [first application](first-application.md): a quick-note form in four steps.

## Start with the declarative API

`<portals/ui.hpp>` provides scenes, recipes and event handlers.
`<portals/ui_style.hpp>` adds the style operations used in this example.

Declare the control tree, then attach style and event handlers with
[UI stages](glossary.md#glossary-ui-stage), the objects describing operations after each `|`.
In the example, `ui::vbox(...)` creates a [recipe](glossary.md#glossary-recipe),
a description of the controls to construct. `ui::scene(...)` stores it in a
[scene](glossary.md#glossary-scene), which manages preparation and running.
The button appends text to the editor; F10 requests an orderly exit.

```cpp
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> message;
  ui::Ref<Button> add;

  auto scene = ui::scene(ui::vbox(
    ui::label("Greeting"),
    ui::lineEdit(message, ""),
    ui::button(add, "Add Hello")));

  return scene
    | ui::style("Button { bold: true; }")
    | ui::onClick(add,
                  [message] {
                    message.text() += "Hello!";
                  })
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

The runner attempts terminal cleanup and reports failures. This compact form
constructs the recipe before `runApp`; use the factory-based overload when
construction also needs its checked error boundary. The exit command resolves the active scene inside a callback entered by the scene.
An explicit `SceneRef` remains available when an action must target a named scene.
[Declarative composition](declarative-ui.md) explains callbacks, setup errors
and the lower-level typed `run()` result.

## Build your application

Create a directory named `my-app`. Save the complete example above as
`main.cpp` and the following configuration as `CMakeLists.txt` in that directory:

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyApplication LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(portals
  GIT_REPOSITORY https://github.com/ttldtor/portals.git
  GIT_TAG 7f09c7db9547c34c547b05e4fbf18d5c9f492ee5
)
FetchContent_MakeAvailable(portals)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE portals::core)
```

Run these commands from `my-app`. CMake downloads the selected Portals revision
during configuration and builds your `my_app` executable:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target my_app
./build/my_app
```

On Windows, use an x64 Developer PowerShell for Visual Studio. On Linux,
use a compatible C++ compiler and standard library. You need Git, CMake 3.24+,
Ninja and a toolchain supporting C99 and C++23, including the C++ library
headers `<expected>` and `<format>`.

Run `my_app` in a terminal: click **Add Hello** to append text, and press F10
to exit. On Windows the executable is `build/my_app.exe`; on Linux it is
`build/my_app`. The commands above use the Ninja single-configuration generator.

`GIT_TAG` fixes the source revision for this example. Change it when choosing
a newer Portals version. FetchContent integration is described in the
[CMake documentation](https://cmake.org/cmake/help/latest/module/FetchContent.html).

### Use local sources or an installed SDK

If you already have Portals sources in a `portals` subdirectory of your project,
replace `include(FetchContent)` and both `FetchContent_*` calls with
`add_subdirectory(portals)`. No download is needed.

For an installed SDK, replace those same lines with
`find_package(Portals CONFIG REQUIRED)` and pass its installation prefix in
`CMAKE_PREFIX_PATH` when configuring. All variants use
`target_link_libraries(my_app PRIVATE portals::core)`. See
[package setup](package.md) for installation and toolchain compatibility.

## What is available

- Forms: Label, Button, CheckBox, LineEdit, Row/Column, Grid/Overlay and observing references.
- Data: immutable models, virtual lists/tables, scrolling and charts.
- Styles: themes, style layers and compile-time/runtime PSS.
- Async: optional, bounded UI coroutines on the existing Scheduler.
- Input: shortcut strings, alternatives, sequences and explicit phase selectors.
  Native delivery depends on the backend; Kitty negotiation and standalone modifier keys are not implemented in this version.
- Errors: live `onError` reactions and a separate final report after cleanup;
  applications can present explicit [notifications](notifications.md), including
  persistent messages and optional indicators. Automatic error popups and a
  diagnostic journal are application policy.

These APIs are evolving. Read [limitations](limitations.md) for qualified
platforms and the exact runtime boundaries.

## Continue reading

- [Dialogs](dialogs.md): open retained modal panels and anchored nonmodal layers.
- [Notifications](notifications.md): show bounded messages and actions while editing continues.
- [First application](first-application.md): build a note form in four steps.
- [Task guides](guides.md): choose a guide for styles, events, data or other application work.
- [Documentation contents](reference-map.md): find a topic or an API contract.
- [Glossary](glossary.md): look up scenes, recipes, UI stages and other terms.
- [Diagnostics](diagnostics.md): find a check and an action for a symptom.

[Runnable examples](examples.md) link to complete source programs.
[Release notes](../ReleaseNotes.md) record changes by version.

[Dependencies and licenses](dependencies.md) are listed separately.

[Fine scene settings](declarative-ui.md#scene-fine-settings) contains
expandable examples of ownership budgets and input settings.
