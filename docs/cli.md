# Command-line launch with Lyra {#cli}

[Русская версия](ru/cli.md)

The optional `portals::lyra` target supplies the pinned, header-only
[Lyra 1.8.0](https://github.com/bfgroup/Lyra/releases/tag/1.8.0) dependency
and `<portals/ui_lyra.hpp>`. Link it explicitly:

```cmake
find_package(Portals CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE portals::lyra)
```

The same target works with `add_subdirectory`. Installed headers, CMake metadata,
license and dependency manifest travel with the SDK and require no network or
system Lyra installation. Core-only consumers link `portals::core` and do not
include or parse Lyra.

Build a native Lyra declaration, bind it to ordinary typed options and pass the
adapter to the lazy launch stage:

```cpp
#include <portals/ui.hpp>
#include <portals/ui_lyra.hpp>

struct Options {
  int width{80};
  std::string theme{"modern"};
  bool ansi16{};
};

int main(int argc, char **argv) {
  namespace ui = portals::ui;
  Options options;
  auto parser = ::lyra::cli()
    | ::lyra::opt(options.width, "columns")["-w"]["--width"]("Requested columns.")
    | ::lyra::opt(options.theme, "theme")["-t"]["--theme"]("Palette.").choices("modern", "classic")
    | ::lyra::opt(options.ansi16)["--ansi16"]("Use 16 ANSI colors.");

  return ui::scene([&](portals::ManagedScene &) {
           return ui::vbox(ui::label(options.theme), ui::label(std::to_string(options.width)));
         })
    | ui::applicationOptions([&] {
           portals::ApplicationOptions settings;

           settings.writer.colorMode = options.ansi16 ? portals::ColorMode::ANSI16 : portals::ColorMode::TRUE_COLOR;

           return settings;
         })
    | ui::onKey(portals::key::F10, ui::quit)
    | ui::reportOnError()
    | ui::runApp(argc, argv, ui::lyra::cli(parser, {.version = "my_application 1.0"}));
}
```

`ui::lyra::cli` owns a copy of the parser declaration. It does not capture argv
or parse during construction. Lyra's bound variables and reference captures
remain borrowed: keep them alive through synchronous `runApp`. The lazy scene
factory observes the parsed options. `ui::opts(options)` can pass the same
read-only typed options into a reusable lazy scene factory.
Parser/recipe construction and owning captures can allocate before the runner
boundary. Callbacks invoked by the runner use its checked error policy.

`CommandLineError::message` and `LaunchAction::output` use `LaunchText`, which
shares immutable owning bytes. Empty construction, copying and moving allocate
nothing, including on MSVC Debug. Its `view()`, `data()`, `size()`, `empty()` and
`find()` accessors are read-only; keep a copy of the text alive while borrowing
its bytes. Constructing nonempty text copies the supplied bytes and can allocate.
When this happens inside the parser callback, allocation failure becomes
`NO_MEMORY` with an empty diagnostic and exit status 2. Text constructed before
the runner call follows the construction boundary described above.

By default the adapter adds Lyra's `-?`, `-h`, `--help`. A nonempty `version`
adds `--version`. These options use Lyra's own short-circuit results and return
an owning `LaunchAction` with `FINISH`; `runApp` prints its output and
returns before invoking the scene factory or acquiring a terminal. There is no
`std::exit`. Do not also declare these option names in the supplied parser.

For custom help, declare `::lyra::help(showHelp)` in the native parser and pass
`{.automaticHelp = false, .helpRequested = &showHelp}`. The flag is borrowed and
must outlive the call. This explicit observer is necessary because Lyra hides
the help flag behind its callback; inspecting argument strings would bypass
the parser. Initialize it to false for each launch.

Invalid numbers, missing values, unknown options and failed native `.choices`
validation become `CommandLineErrorCode::INVALID_ARGUMENT` with an owning
Lyra diagnostic; `runApp` returns status 2 for command-line failure. The generic
launch boundary handles parser callback exceptions separately from widget
errors. Lyra can update some bound values
or invoke callbacks before a later parse failure; the adapter preserves that
upstream behavior and never constructs the application after failure.

Lyra consumes the strings supplied by the launch argument snapshot. It does
not convert Windows narrow `main` arguments into Unicode. Non-ASCII command
arguments therefore depend on how the executable obtains its argv; the
Windows Unicode environment snapshot is a separate facility and does not
repair narrow argv. POSIX argument bytes are preserved.

`ui::app::args()`, `ui::app::environment()` and its alias `ui::app::env()` return
checked owning read-only snapshots after successful `ui::app::initialize`,
including initialization performed by `ui::runApp(argc, argv, parser)`.
Before initialization they return `ProcessContextError::UNINITIALIZED`.
`Environment::get(name)` returns
`std::expected<std::optional<std::string_view>, ProcessContextError>` without
throwing: an error reports an invalid query or capture/conversion failure,
successful `std::nullopt` means the variable is missing, and an engaged empty
view means an existing empty value. Keep the snapshot alive while borrowing its
views. Windows lookup compares Unicode names without case; POSIX preserves
native bytes and compares names exactly. Reading a snapshot never refreshes or
mutates the process environment. Capture it before other threads can mutate the
live environment; Portals cannot synchronize foreign environment writers.

Lyra is licensed under BSL-1.0, independently of Portals. The unchanged notice
is installed as `share/Portals/licenses/lyra-LICENSE.txt`; the vendored
`vendor/lyra/manifest.json` records provenance and checksums.
See the [native Lyra reference](https://www.bfgroup.xyz/Lyra/lyra.html) for
declarations, callbacks, validation, option styles and parse guarantees.
