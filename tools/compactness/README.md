# Native source compactness tool

Build `portals_compactness` and `portals_compactness_tests` with CMake. Neither
target links the Portals core; the tool needs only a C++23 standard library.
Run `portals_compactness --help` for the CLI contract.

Repeat `--before FILE` and `--after FILE` to include all complete client files
and local/shared helpers. Canonical duplicate paths count once per side.
`--format json|markdown` selects a deterministic report. This bounded lexer
reports `lexical_tokens_approx` and supporting measurements; UI slices are
`NOT_MEASURED`, equivalence is `NOT_CHECKED`, and no overall score is computed.

See [the full contract and build instructions](../../docs/compactness.md).
Primary references for complementary semantic/complexity analysis:
[Clang LibTooling](https://clang.llvm.org/docs/LibTooling.html),
[Clang-tidy cognitive complexity](https://clang.llvm.org/extra/clang-tidy/checks/readability/function-cognitive-complexity.html),
and [Lizard](https://github.com/terryyin/lizard).
