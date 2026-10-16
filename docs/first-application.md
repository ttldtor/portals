# First application: a quick note {#first-application}

[Русская версия](ru/first-application.md) · [User guides](guides.md) · [Glossary](glossary.md)

Build an application with an editor and **Save** and **Clear draft** buttons.
In four steps, you will describe a form, connect actions and add styling.
**Save** copies the editor text to the line below it. The note lasts until the
application exits; this example does not write a file.

## 1. Create and run a form

You need Git, CMake 3.24+, Ninja and a toolchain supporting C99 and C++23,
including `<expected>` and `<format>`. On Windows, open an x64 Developer
PowerShell for Visual Studio. See the [short guide](index.md) for integration details.

Create a `quick-note` directory and save this code as `main.cpp`:

<details open>
<summary>Complete main.cpp after step 1</summary>

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> draft;

  auto scene = ui::scene(ui::vbox(
    ui::label("Quick note"),
    ui::lineEdit(draft, "Remember the milk"),
    ui::label("Tab: focus | F10: quit")));

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

</details>

`ui::vbox` describes a vertical arrangement of controls. This [recipe](glossary.md#glossary-recipe)
is stored in a [scene](glossary.md#glossary-scene) by `ui::scene`.
`ui::Ref<LineEdit> draft` provides access to the created editor. Run the program
to edit **Remember the milk**; F10 exits.

Create `CMakeLists.txt` in the same directory:

```cmake
cmake_minimum_required(VERSION 3.24)
project(QuickNote LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(portals
  GIT_REPOSITORY https://github.com/ttldtor/portals.git
  GIT_TAG cb0fdec5634cfc9d877791313bdee5da2c6aa18f
)
FetchContent_MakeAvailable(portals)

add_executable(quick_note main.cpp)
target_link_libraries(quick_note PRIVATE portals::core)
```

Run these commands from `quick-note`:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target quick_note
./build/quick_note
```

On Windows, the file is `build/quick_note.exe`. Run it in a terminal.
CMake downloads the pinned Portals revision during the first configuration.
To use an existing local checkout, add
`-DFETCHCONTENT_SOURCE_DIR_PORTALS="C:/path/to/portals"` to the configure command,
substituting your path. [Complete step 1 source](examples/tutorial_note_1.cpp).

## 2. Display the saved text

Exit the application. Continue editing the same `main.cpp` from step 1.
After `ui::Ref<LineEdit> draft;`, add references to the label and button:

```cpp
  ui::Ref<Label> saved;
  ui::Ref<Button> save;
```

Inside `ui::vbox(...)`, immediately after `ui::lineEdit(draft, "Remember the milk"),`,
add these two controls. Keep the final hint label after them:

```cpp
    ui::label(saved, "Nothing saved yet"),
    ui::button(save, "Save"),
```

Between the `scene` declaration and `return scene`, create the `saveNote` action.
In the chain after `return scene`, add `ui::onClick(save, saveNote)` before
`ui::onKey("F10", ui::quit)`. This part of `main` becomes:

```cpp
  auto saveNote = [draft, saved] {
    saved.text() = draft.text();
  };

  return scene
    | ui::onClick(save, saveNote)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
```

`saveNote` reads the current editor text and assigns it to the label.
The `|` operator attaches a [UI stage](glossary.md#glossary-ui-stage) to the scene;
`ui::onClick` connects the button to an action. Capture references by value in
handlers: they observe the scene's controls and do not extend their lifetime.
See [control references](text-property.md) for details.

<details>
<summary>Complete main.cpp after step 2</summary>

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> draft;
  ui::Ref<Label> saved;
  ui::Ref<Button> save;

  auto scene = ui::scene(ui::vbox(
    ui::label("Quick note"),
    ui::lineEdit(draft, "Remember the milk"),
    ui::label(saved, "Nothing saved yet"),
    ui::button(save, "Save"),
    ui::label("Tab: focus | F10: quit")));

  auto saveNote = [draft, saved] {
    saved.text() = draft.text();
  };

  return scene
    | ui::onClick(save, saveNote)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

</details>

[Download the step 2 source](examples/tutorial_note_2.cpp).

Repeat the build command from step 1 and run the application. Edit the text,
press Tab to focus **Save**, then Enter. The note appears below the editor.
Further editing leaves that line unchanged until the next **Save**.

## 3. Add clearing and shortcuts

Exit the application. In `main.cpp` from step 2, after `ui::Ref<Button> save;`,
add a reference to the clear button:

```cpp
  ui::Ref<Button> clear;
```

Inside `ui::vbox(...)`, add **Clear draft** after **Save** and update the final
hint label. The `scene` declaration now looks like this:

```cpp
  auto scene = ui::scene(ui::vbox(
    ui::label("Quick note"),
    ui::lineEdit(draft, "Remember the milk"),
    ui::label(saved, "Nothing saved yet"),
    ui::button(save, "Save"),
    ui::button(clear, "Clear draft"),
    ui::label("Tab: focus | F2: save | F3: clear | F10: quit")));
```

After the existing `saveNote` action, declare `clearDraft`.
Extend the handler chain: connect the new button to clearing, F2 to saving,
and F3 to clearing. The section from `clearDraft` to the end of the chain is:

```cpp
  auto clearDraft = [draft] {
    draft.text() = "";
  };

  return scene
    | ui::onClick(save, saveNote)
    | ui::onClick(clear, clearDraft)
    | ui::onKey("F2", saveNote)
    | ui::onKey("F3", clearDraft)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
```

`clearDraft` clears only the editor; the saved line stays on screen.
The **Clear draft** button and F3 invoke the same action. The **Save** button
and F2 invoke the other action, `saveNote`, created in the previous step.

<details>
<summary>Complete main.cpp after step 3</summary>

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> draft;
  ui::Ref<Label> saved;
  ui::Ref<Button> save;
  ui::Ref<Button> clear;

  auto scene = ui::scene(ui::vbox(
    ui::label("Quick note"),
    ui::lineEdit(draft, "Remember the milk"),
    ui::label(saved, "Nothing saved yet"),
    ui::button(save, "Save"),
    ui::button(clear, "Clear draft"),
    ui::label("Tab: focus | F2: save | F3: clear | F10: quit")));

  auto saveNote = [draft, saved] {
    saved.text() = draft.text();
  };

  auto clearDraft = [draft] {
    draft.text() = "";
  };

  return scene
    | ui::onClick(save, saveNote)
    | ui::onClick(clear, clearDraft)
    | ui::onKey("F2", saveNote)
    | ui::onKey("F3", clearDraft)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

</details>

[Download the step 3 source](examples/tutorial_note_3.cpp).

Build and run again. Press F2, then F3: the editor becomes empty and the saved
note remains. Type new text and press F2 to replace it. Tab moves focus between
the editor and buttons; Enter activates the focused button. See the
[input guide](input.md) for other shortcut forms.

## 4. Add styling

Exit the application. At the start of `main.cpp` from step 3, after
`#include <portals/ui.hpp>`, add the header for styling operations:

```cpp
#include <portals/ui_style.hpp>
```

In the chain after `return scene`, before `ui::onClick(save, saveNote)`,
add these two stages:

```cpp
    | ui::theme(nordTheme())
    | ui::style("Button { min-width: 16; bold: true; }")
```

`ui::theme(nordTheme())` selects a palette. The PSS rule makes button text bold
and sets a minimum width of 16 terminal cells. Keep the references, form
and handlers from step 3 in place. See [styles](styling-guide.md) and
[themes](themes-guide.md) for more options.

<details>
<summary>Complete main.cpp after step 4</summary>

```cpp
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> draft;
  ui::Ref<Label> saved;
  ui::Ref<Button> save;
  ui::Ref<Button> clear;

  auto scene = ui::scene(ui::vbox(
    ui::label("Quick note"),
    ui::lineEdit(draft, "Remember the milk"),
    ui::label(saved, "Nothing saved yet"),
    ui::button(save, "Save"),
    ui::button(clear, "Clear draft"),
    ui::label("Tab: focus | F2: save | F3: clear | F10: quit")));

  auto saveNote = [draft, saved] {
    saved.text() = draft.text();
  };

  auto clearDraft = [draft] {
    draft.text() = "";
  };

  return scene
    | ui::theme(nordTheme())
    | ui::style("Button { min-width: 16; bold: true; }")
    | ui::onClick(save, saveNote)
    | ui::onClick(clear, clearDraft)
    | ui::onKey("F2", saveNote)
    | ui::onKey("F3", clearDraft)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

</details>

[Download the step 4 source](examples/tutorial_note_4.cpp).
Build with the same command and run the application to see the styling.

## Result

![Note form after Save: Remember the milk in the editor and saved line, with Save and Clear draft buttons.](images/first-application.png)

The image shows output from the final program in a 64 × 10 in-memory terminal
after **Save**, using the Nord theme. It is displayed through xterm.js;
your terminal's font and colors may differ.

If startup fails, `ui::runApp()` reports the error to stderr after attempting
terminal cleanup and returns a nonzero exit code. Description construction
before `runApp()` follows ordinary C++ exception rules; use the
[factory overload](declarative-ui.md) to include that step in the error boundary.
[Diagnostics](diagnostics.md) covers default and custom reports.

Next, [add shared state](property.md) or [change the layout](layout.md).
Choose a [user guide](guides.md) for your next task.
