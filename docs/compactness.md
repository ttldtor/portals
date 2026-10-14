# Source analysis tool

`portals_compactness` is a developer tool for comparing explicitly supplied C++
source files. It reports lexical measurements in JSON or Markdown.
The tool requires a C++23 standard library and builds independently of Portals core.

Build the `portals_compactness` target with CMake, then run
`portals_compactness --help` for the command-line options.
Repeated `--before FILE` and `--after FILE` arguments select the two file sets.
Include application helpers in each set; duplicate canonical paths count once.

The scanner measures source text. Use tests to check behavior and benchmarks to
measure runtime costs. Source measurements do not check API equivalence.

The [tool sources](../tools/compactness/compactness.hpp) document the report types.
[Regression tests](../tests/compactness.cpp) exercise scanning and reporting.

For application examples, start with [runnable examples](examples.md).
