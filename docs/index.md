# Portals: short library guide

[Русская версия](ru/index.md)

Portals is a C++23 retained terminal UI library under development.
The declarative `ui` API is the primary entry point for application code.
This page is the short reading guide; the [reference map](reference-map.md)
contains the complete topic list and API reading paths.

## Start with the declarative API

Declare the control tree, then attach style and event handlers with pipe stages.
The button appends text to the editor; F10 requests an orderly exit.

```cpp
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
    | ui::onClick(add, [message] { message.text() += "Hello!"; })
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

The runner attempts terminal cleanup and reports failures. This compact form
constructs the recipe before `runApp`; use the factory-based overload when
construction also needs its checked error boundary. The exit command resolves the active scene inside its admitted callback.
An explicit `SceneRef` remains available when an action must target a named scene.
[Declarative composition](declarative-ui.md) explains callbacks, setup errors
and the lower-level typed `run()` result.

## Build and run

Use an x64 Developer PowerShell for Visual Studio on Windows:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
./build/windows-msvc-debug/examples/gallery/portals_gallery
```

On Linux use `linux-gcc-debug` and the matching build directory.
[Getting started](getting-started.md) covers compiler requirements, tests and local HTML help.

## What is available

- Forms: Label, Button, CheckBox, LineEdit, Row/Column, Grid/Overlay and observing references.
- Data: immutable models, virtual lists/tables, scrolling and charts.
- Styles: themes, style layers and compile-time/runtime PSS.
- Async: optional, bounded UI coroutines on the existing Scheduler.
- Input: shortcut strings, alternatives, sequences and explicit phase selectors.
  Native delivery depends on the backend; Kitty negotiation and standalone modifier keys are not implemented in this version.
- Errors: live `onError` reactions and a separate final report after cleanup;
  no built-in diagnostic indicator or panel is provided yet.

These APIs are evolving. Read [limitations](limitations.md) for qualified
platforms and the exact runtime boundaries.

## User guides

[Contents and subject index](guides.md) · [Glossary](glossary.md)

| Task | Guide |
|---|---|
| Compose a form and handle errors | [Declarative UI](declarative-ui.md) |
| Prepare styles and themes | [Styling guide](styling-guide.md), [themes guide](themes-guide.md) |
| Bind state and handle events | [Bindings and events](bindings-guide.md) |
| Load scene text/files (TBD — M1/U84) | [Scene loading](scene-loading-guide.md) |
| Bind keys and understand input phases | [Input](input.md) |
| Use an existing CMake project | [Package](package.md) |
| Understand ownership and frame flow | [Architecture](architecture.md) |
| Find another topic or API declaration | [Reference map](reference-map.md) |

[Runnable examples](examples.md) link to complete source programs.
[Release notes](../ReleaseNotes.md) record changes by version; design sketches
and measurement comparisons describe their own dated scope.
