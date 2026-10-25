#!/usr/bin/env python3
"""Check project C++ names with clang-tidy and a configured compile database."""

import argparse
from collections import deque
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from time import perf_counter


def positive_jobs(value):
    jobs = int(value)
    if jobs < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return jobs


def check_source(source, executable, build, root, output):
    """Pool worker: spool one invocation; return status/time or propagate I/O errors."""
    started = perf_counter()
    with output.with_suffix(".stdout").open("wb") as stdout, output.with_suffix(".stderr").open("wb") as stderr:
        result = subprocess.run([
            executable, str(source), "-p", str(build), "--quiet",
            f"--config-file={root / '.clang-tidy'}",
        ], cwd=root, stdout=stdout, stderr=stderr, check=False)
    return result.returncode, perf_counter() - started


def replay_output(path, stream):
    """Called by main to copy a spool file to a console stream, then remove it."""
    stream.flush()
    with path.open("rb") as output:
        shutil.copyfileobj(output, stream.buffer)
    stream.flush()
    path.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__, epilog="Requires the configured compiler environment and project .clang-tidy. Runs all selected translation units without fixes, even after findings. Diagnostics use temporary files, removed on exit, and are replayed in source order. Exit 0: naming checks passed; 1: findings or tool/input/report failure; 2: invalid CLI syntax or no selected sources.")
    parser.add_argument("build", type=Path, help="Directory containing compile_commands.json")
    parser.add_argument("--clang-tidy", default="clang-tidy", help="Executable name on PATH or full path (default: clang-tidy)")
    parser.add_argument("--jobs", type=positive_jobs, default=1, help="Maximum concurrent clang-tidy processes, positive integer (default: 1)")
    parser.add_argument("--timings", type=Path, help="Overwrite this JSON file with total/per-TU wall seconds, return codes and jobs (default: no report; parent directory must exist)")
    args = parser.parse_args()
    started = perf_counter()
    root = Path(__file__).resolve().parents[1]
    build = args.build.resolve()
    entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    files = set()
    for entry in entries:
        source = Path(entry["file"])
        if not source.is_absolute():
            source = Path(entry["directory"]) / source
        source = source.resolve()
        if not source.is_relative_to(root):
            continue
        relative = source.relative_to(root)
        # These fixtures deliberately violate compile-time contracts. Their
        # expected diagnostics are checked by dedicated CTest cases.
        if relative.as_posix() in {
            "tests/fixtures/static_scene/invalid.cpp",
            "tests/fixtures/static_styles/invalid.cpp",
            "tests/fixtures/ct_pss/invalid.cpp",
        }:
            continue
        if relative.parts[0] in {"src", "tests", "tools", "examples", "experiments", "fuzz"}:
            files.add(source)
    if not files:
        parser.error("The compilation database contains no project C++ sources")
    failed = []
    timings = []
    sources = iter(enumerate(sorted(files)))
    # Keep at most jobs results on disk, including completed results waiting
    # for an earlier source. Worker threads only wait for external processes.
    with tempfile.TemporaryDirectory(prefix="check-naming-") as temporary:
        with ThreadPoolExecutor(max_workers=args.jobs) as executor:
            pending = deque()

            def submit_next():
                item = next(sources, None)
                if item is None:
                    return
                index, source = item
                output = Path(temporary) / str(index)
                future = executor.submit(check_source, source, args.clang_tidy, build, root, output)
                pending.append((source, output, future))

            for _ in range(min(args.jobs, len(files))):
                submit_next()
            while pending:
                source, output, future = pending.popleft()
                print(f"Checking {source.relative_to(root)}", flush=True)
                returncode, seconds = future.result()
                replay_output(output.with_suffix(".stdout"), sys.stdout)
                replay_output(output.with_suffix(".stderr"), sys.stderr)
                if returncode:
                    failed.append(source)
                timings.append(dict(source=source.relative_to(root).as_posix(),
                                    seconds=seconds, returncode=returncode))
                submit_next()
    print(f"Checked {len(files)} translation units; {len(failed)} failed.")
    if args.timings:
        args.timings.write_text(json.dumps(dict(jobs=args.jobs,
                                               total_seconds=perf_counter() - started,
                                               translation_units=timings), indent=2) + "\n",
                                encoding="utf-8")
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
