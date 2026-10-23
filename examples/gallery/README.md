# Control gallery

[`form`](fluent_form.hpp) is an ordinary function returning a lazy owning scene.
It describes labels, an explicit-mode text field, a checkbox and three buttons,
then applies styles and handlers through pipe modifiers. It has no test transport,
statistics, counter or stationary wrapper object. The optional Run callback is an
application action; by default Run leaves the field models unchanged.

```cpp
example::Options options;
auto lyraCli = example::commandLine(options);

return gallery::form(ui::opts(options))
  | ui::reportOnError()
  | ui::runApp(argc, argv, ui::lyra::cli(lyraCli));
```

The generic runner invokes the native Lyra schema before it creates Application,
reads writer settings, mounts controls or opens a terminal. Help returns zero;
invalid options report an owning CLI diagnostic and return two. The Options object
must outlive the synchronous run. Schema/recipe construction can allocate before
the runner boundary; user callbacks invoked inside the boundary are checked.

[`settings.hpp`](settings.hpp) defines the two settings; [`options.hpp`](options.hpp)
defines their native Lyra schema, with no handwritten argv loop. The separate
[`themes.hpp`](themes.hpp) preserves the existing preset order and F2 behavior;
the scene-owned selector previews themes before committing them. Enter commits the
name, Tab changes focus, F2 changes theme, F10 or Ctrl+C quits.

```powershell
.\portals_gallery.exe --theme nord
.\portals_gallery.exe --ansi16
.\portals_gallery.exe --help
```

The core scene does not depend on Lyra. Link the CLI executable with `portals::lyra`;
Lyra headers, license and manifest are included in the offline SDK. See the
[CLI guide](../../docs/cli.md) and [examples guide](../../docs/examples.md).

The former stationary form wrapper is now test-owned in
[`tests/support/gallery_fluent.hpp`](../../tests/support/gallery_fluent.hpp).
Finite transports and checks remain in tests.
