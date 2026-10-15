#!/usr/bin/env python3
# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
"""Check benchmark help with real binaries without running benchmark workloads.

Run with --bin-dir pointing at the CMake runtime output directory. Each child is
isolated in an empty temporary directory, has a ten-second timeout, and must not
create files. This driver returns 0 when all checks pass and 1 on a failed check.
"""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


TOOLS = {
    "managed": ("--samples", "--warmup", "--rows", "--check", "--output"),
    "style_layers": ("--samples", "--warmup", "--rows", "--check", "--output"),
    "pss": ("--samples", "--warmup", "--batch", "--reverse", "--check", "--output"),
    "mvp": (
        "--output", "--transport", "--diagnostics", "--rate", "--seed", "--rows",
        "--fps", "--warmup-ms", "--duration-ms", "--delay-ms", "--sample-capacity",
        "--ascii", "--ansi16", "--encoding-only",
    ),
    "compact": (),
}


def checkTool(executable: Path, options: tuple[str, ...], directory: Path) -> int:
    """Require standalone help and reject mixed help before any side effects."""
    checks = 0
    for flag in ("--help", "-h"):
        result = subprocess.run(
            [str(executable), flag], cwd=directory, capture_output=True,
            text=True, timeout=10, check=False,
        )
        assert result.returncode == 0, (executable.name, flag, result.stderr)
        assert not result.stderr, (executable.name, flag, result.stderr)
        for text in ("Usage:", "Options:", "Output:", "Exit:", "--help", "-h", *options):
            assert text in result.stdout, (executable.name, flag, "missing help text", text)
        assert not list(directory.iterdir()), (executable.name, flag, "created files")
        checks += 1

    report = directory / "must-not-create.json"
    for arguments in (
        ["--output", str(report), "--help"],
        ["-h", "--output", str(report)],
        ["--help", "--unknown"],
        ["--samples", "invalid", "--help"],
    ):
        result = subprocess.run(
            [str(executable), *arguments], cwd=directory, capture_output=True,
            text=True, timeout=10, check=False,
        )
        assert result.returncode == 1, (executable.name, arguments, result.returncode)
        assert "sole argument" in result.stderr, (executable.name, arguments, result.stderr)
        assert not result.stdout, (executable.name, arguments, "unexpected stdout")
        assert not list(directory.iterdir()), (executable.name, arguments, "created files")
        checks += 1
    return checks


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bin-dir", required=True, type=Path,
        help="CMake runtime output directory containing all five benchmark executables",
    )
    arguments = parser.parse_args()
    checks = 0
    with tempfile.TemporaryDirectory(prefix="portals-tool-help-") as temporary:
        for name, options in TOOLS.items():
            executable = arguments.bin_dir.resolve() / f"portals_{name}_benchmark"
            if not executable.is_file():
                executable = executable.with_suffix(".exe")
            assert executable.is_file(), f"Missing executable: {executable}"
            checks += checkTool(executable, options, Path(temporary))
    print(f"Passed {checks} benchmark help checks across {len(TOOLS)} executables.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"Benchmark help check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
