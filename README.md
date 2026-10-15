# Portals

[Русская документация](docs/ru/index.md) · [User guides](docs/guides.md)

[![Build and test](https://github.com/ttldtor/portals/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/ttldtor/portals/actions/workflows/ci.yml)
[![Documentation](https://img.shields.io/badge/docs-online-blue)](https://ttldtor.github.io/portals/)
![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=c%2B%2B)
[![License: BSL 1.0](https://img.shields.io/badge/License-BSL--1.0-blue.svg)](LICENSE.txt)

Portals is a **C++23 terminal UI library**, currently under development.
Start with the declarative `ui` API to compose forms, bind events and run scenes.
The library provides widgets, layouts, managed controls, virtual lists and
tables, and charts. The API and ABI are
evolving; see the [current limitations](docs/limitations.md) before depending
on platform or lifecycle guarantees.

Read the [online documentation and API reference](https://ttldtor.github.io/portals/)
for guides, runnable examples, and public API contracts.

## Declarative API

Declare the control tree, style and event handlers in one composition.
The button appends text; F10 requests an orderly exit:

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

This compact form constructs the [recipe](docs/glossary.md#glossary-recipe) before `runApp`; the
[factory-based runner](docs/declarative-ui.md) also guards recipe construction.

Start with the [short library guide](docs/index.md), then follow its links for
input, error reporting and ownership contracts.

## Create your application

Save the example above as `main.cpp`. The [short guide](docs/index.md) provides
a complete CMake project using FetchContent, plus configure, build and run commands.
It also covers local sources and an installed SDK. For runnable library examples,
see [the examples page](docs/examples.md).

## Documentation

[Русские гайды](docs/ru/guides.md) · [Краткое руководство](docs/ru/index.md)

- [Online documentation and API reference](https://ttldtor.github.io/portals/) — published through GitHub Actions.
- [Short library guide](docs/index.md) — declarative API first.
- [User guides](docs/guides.md) — contents, subject index and planned milestones.
- [Glossary](docs/glossary.md) — terms and ownership distinctions.
- [Declarative UI](docs/declarative-ui.md) — composition, events and scene lifecycle.
- [Reference map](docs/reference-map.md) — complete topic map.
- [Building Portals and its examples](docs/getting-started.md) — build, run examples, and generate local HTML help.
- [Runnable examples](docs/examples.md) — gallery, dashboard, custom widgets, and options.
- [Architecture](docs/architecture.md) — ownership and event flow.
- [CMake package](docs/package.md) — embed from source or install for consumers.
- [Limitations](docs/limitations.md) — supported platforms and runtime boundaries.
- [Development guide](docs/development.md) — testing, design material, and documentation upkeep.
- [Release notes](ReleaseNotes.md) — implemented changes and release status.

## License and notices

Portals is licensed under the [Boost Software License 1.0](LICENSE.txt).
[Dependencies and licenses](docs/dependencies.md) describes included third-party
code, data, optional components and build requirements.
[Зависимости и лицензии на русском](docs/ru/dependencies.md).
