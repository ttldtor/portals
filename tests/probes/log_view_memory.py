#!/usr/bin/env python3
# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
"""Generate filled fixtures and run the external LogView memory acceptance probe.

Help: python tests/probes/log_view_memory.py --help
Smoke: python tests/probes/log_view_memory.py --exe BUILD/log_view_memory.exe --work-dir WORK --output WORK/result.json --sizes-mib 1
Full: omit --sizes-mib for independent 10/100/1024 MiB inputs, three runs each.
Only explicitly owned generated fixture files are removed; raw JSON, stdout/stderr,
hashes and a 64 KiB review fixture remain. No build, Git, OS settings or other process is touched.
Exit 0: all applicable gates pass; 1: failed gate/run; 2: argument/preflight failure.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

MIB = 1024 * 1024
GIB = 1024 * MIB
ROOT_BYTES = 8 * MIB
PRIVATE_SPREAD_BYTES = 8 * MIB
PEAK_ALLOWANCE_BYTES = ROOT_BYTES + 32 * MIB
RECORD_BYTES = 128
SEED = 32020261105
PRESENT = b"PRTLS_320_NEAR_END_SENTINEL"
ABSENT = b"PRTLS_320_ABSENT_SENTINEL"
FIXTURE_PREFIX = "prtls-320-filled-"
DEFAULT_RUN_TIMEOUT_SECONDS = 600


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate real filled UTF-8 files and run isolated external LogView memory acceptance.",
        epilog="Defaults: 10,100,1024 MiB; 3 sequential independent processes per size; 128-byte records. "
        "Requires each fixture size plus 5 GiB free. Removes only exact generated large files; retains evidence. "
        "Exit: 0 gates passed, 1 failed checks/run, 2 invalid arguments/preflight. --help performs no work.",
    )
    parser.add_argument("--exe", type=Path, required=True, help="Already-built production-linked C++ probe executable.")
    parser.add_argument("--work-dir", type=Path, required=True, help="Dedicated existing or new directory for owned fixtures/evidence.")
    parser.add_argument("--output", type=Path, required=True, help="Aggregate JSON evidence path (must lie inside work-dir).")
    parser.add_argument("--sizes-mib", type=int, nargs="+", default=[10, 100, 1024],
                        help="Explicit smaller sizes for smoke only; default: 10 100 1024.")
    parser.add_argument("--toolchain", default="unknown", help="Coordinator-supplied build/configuration identity; default unknown.")
    parser.add_argument("--run-timeout", "--run-timeout-seconds", dest="run_timeout_seconds", type=int,
                        default=DEFAULT_RUN_TIMEOUT_SECONDS,
                        help="Hard subprocess deadline in seconds; only this helper's timed-out child is terminated (default 600).")
    return parser.parse_args()


def no_links(path: Path) -> None:
    for candidate in [path, *path.parents]:
        if candidate.is_symlink() or (hasattr(candidate, "is_junction") and candidate.is_junction()):
            raise ValueError(f"Symlink/junction is not an owned plain path: {candidate}")


def contained(path: Path, root: Path) -> Path:
    no_links(path)
    result = path.resolve(strict=False)
    if not result.is_relative_to(root) or result == root:
        raise ValueError(f"Target escapes dedicated work directory: {result}")
    return result


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(MIB):
            value.update(block)
    return value.hexdigest()


def record(index: int, sentinel: bool = False) -> bytes:
    # Content varies deterministically; UTF-8 lengths and grapheme boundaries differ while
    # on-disk records stay exactly 128 bytes, making byte-aligned oracle positions independent.
    unicode_text = ["café", "東京", "a\u0301", "🙂"][index % 4]
    message = ("detail=" + "x" * (1 + index % 19)).encode("ascii")
    prefix = f"record={index:012d} seed={SEED} ".encode("ascii")
    body = prefix + unicode_text.encode("utf-8") + b" " + message
    if sentinel:
        body += b" " + PRESENT
    if len(body) >= RECORD_BYTES:
        raise ValueError("Fixture record exceeds the fixed record size")
    return body + b" " * (RECORD_BYTES - 1 - len(body)) + b"\n"


def generate(path: Path, size: int) -> dict:
    if path.exists():
        raise ValueError(f"Refusing to overwrite an existing fixture: {path}")
    if size % RECORD_BYTES:
        raise ValueError("Fixture bytes must be record aligned")
    value = hashlib.sha256()
    count = size // RECORD_BYTES
    started = time.monotonic()
    # At most a 1 MiB write buffer and one record; no file-sized allocation or sparse truncate.
    with path.open("xb") as stream:
        buffer = bytearray()
        for index in range(count):
            buffer.extend(record(index, index == count - 1))
            if len(buffer) >= MIB:
                stream.write(buffer)
                value.update(buffer)
                buffer.clear()
        if buffer:
            stream.write(buffer)
            value.update(buffer)
        stream.flush()
        os.fsync(stream.fileno())
    actual = path.stat().st_size
    if actual != size:
        raise ValueError(f"Filled fixture size mismatch: {actual} != {size}")
    return {"path_name": path.name, "seed": SEED, "bytes": actual, "records": count,
            "record_bytes": RECORD_BYTES, "sha256": value.hexdigest(), "filled_by_writes": True,
            "generation_seconds": time.monotonic() - started,
            "sentinel_record": count - 1, "absent_literal": ABSENT.decode("ascii")}


def evaluate(runs: list[dict], full: bool) -> dict:
    checks: list[dict] = []

    def check(name: str, passed: bool, **evidence) -> None:
        checks.append({"name": name, "status": "PASS" if passed else "FAIL", **evidence})

    check("all_processes_passed", all(run.get("exit_code") == 0 and run.get("raw", {}).get("status") == "PASS"
                                      for run in runs))
    raw_runs = [run["raw"] for run in runs if run.get("raw", {}).get("status") == "PASS"]
    warm_private: list[int] = []
    peak_deltas: list[int] = []
    resident_warm: list[int] = []
    source_highwater = {"bytes": 0, "pages": 0, "work": 0, "prepared_rows": 0, "prepared_bytes": 0}
    metrics = sorted({raw["private_metric"] for raw in raw_runs})

    for raw in raw_runs:
        samples = raw["samples"]
        check("bootstrap_present", bool(samples) and samples[0]["stage"] == "bootstrap")
        if not samples:
            continue
        check("actual_viewport", raw.get("viewport") == [80, 24])
        check("bounded_source_index", raw.get("source_index_bytes") == 0 and raw.get("source_index_kind") == "none")
        check("hard_source_budget", all(s["source_bytes"] <= ROOT_BYTES and s["source_pages"] <= 32
                                        and s["source_work"] <= 4 for s in samples), threshold_bytes=ROOT_BYTES)
        warm = [s for s in samples if s["stage"] not in {"bootstrap", "first_window"}]
        warm_private.append(max(s["private_bytes"] for s in warm))
        resident_warm.append(max(s["resident_bytes"] for s in warm))
        peak = max(max(s["private_bytes"], s["peak_private_bytes"] or s["private_bytes"]) for s in samples)
        delta = max(0, peak - samples[0]["private_bytes"])
        peak_deltas.append(delta)
        check("private_peak_delta", delta <= PEAK_ALLOWANCE_BYTES, observed_bytes=delta,
              threshold_bytes=PEAK_ALLOWANCE_BYTES, kernel_peak_available=raw.get("peak_private_metric") is not None)
        for sample in samples:
            source_highwater["bytes"] = max(source_highwater["bytes"], sample["source_bytes"])
            source_highwater["pages"] = max(source_highwater["pages"], sample["source_pages"])
            source_highwater["work"] = max(source_highwater["work"], sample["source_work"])
            source_highwater["prepared_rows"] = max(source_highwater["prepared_rows"], sample["prepared_rows"])
            source_highwater["prepared_bytes"] = max(source_highwater["prepared_bytes"], sample["prepared_bytes"])

    spread = max(warm_private) - min(warm_private) if warm_private else None
    check("comparable_private_metric", len(metrics) == 1, metrics=metrics)
    if full:
        check("warm_private_spread_across_sizes_and_repeats", spread is not None and spread <= PRIVATE_SPREAD_BYTES,
              observed_bytes=spread, threshold_bytes=PRIVATE_SPREAD_BYTES)
    else:
        checks.append({"name": "warm_private_spread_across_required_sizes", "status": "NOT_CHECKED",
                       "reason": "Explicit smoke sizes do not provide the required 10/100/1024 MiB matrix.",
                       "observed_smoke_spread_bytes": spread})
    return {"status": "FAIL" if any(c["status"] == "FAIL" for c in checks) else "PASS",
            "acceptance_matrix_complete": full, "checks": checks, "source_highwater": source_highwater,
            "warm_private_highwaters_bytes": warm_private, "private_peak_deltas_bytes": peak_deltas,
            "warm_resident_highwaters_bytes": resident_warm,
            "os_file_cache": "Not attributed to this process; filesystem cache state is not controlled.",
            "linux_private_limit": "Linux reports private writable mapping virtual size, not Windows committed PrivateUsage; "
                                   "kernel private peak is unavailable there and sampled highwater is labeled explicitly."}


def main() -> int:
    args = arguments()  # argparse --help exits before path checks or any generation/process launch.
    if any(size < 1 for size in args.sizes_mib) or len(set(args.sizes_mib)) != len(args.sizes_mib):
        raise ValueError("Sizes must be unique positive MiB values")
    if args.run_timeout_seconds <= 0:
        raise ValueError("Process timeout must be positive")
    no_links(args.exe)
    exe = args.exe.resolve(strict=True)
    if not exe.is_file():
        raise ValueError("--exe must name an existing executable file")
    no_links(args.work_dir)
    root = args.work_dir.resolve(strict=False)
    root.mkdir(parents=True, exist_ok=True)
    no_links(root)
    output = contained(args.output, root)
    if output.exists():
        raise ValueError(f"Refusing to overwrite aggregate evidence: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    evidence = {"schema": 1, "status": "RUNNING", "units": "bytes", "toolchain": args.toolchain,
                "exe": str(exe), "exe_sha256": digest(exe), "python": sys.version,
                "sizes_mib": args.sizes_mib, "repeats": 3, "fixtures": [], "runs": [],
                "thresholds": {"source_bytes": ROOT_BYTES, "warm_private_spread_bytes": PRIVATE_SPREAD_BYTES,
                               "peak_delta_bytes": PEAK_ALLOWANCE_BYTES},
                "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    review = contained(root / "prtls-320-review-64k.log", root)
    if review.exists():
        raise ValueError(f"Refusing to overwrite retained review fixture: {review}")
    evidence["review_fixture"] = generate(review, 64 * 1024)

    try:
        for size_mib in args.sizes_mib:
            size = size_mib * MIB
            available = shutil.disk_usage(root).free
            if available < size + 5 * GIB:
                raise ValueError(f"Need fixture + 5 GiB free: available={available}, required={size + 5 * GIB}")
            fixture = contained(root / f"{FIXTURE_PREFIX}{size_mib}mib.log", root)
            receipt = generate(fixture, size)
            receipt["free_bytes_before"] = available
            evidence["fixtures"].append(receipt)
            try:
                for repeat in range(1, 4):
                    if digest(exe) != evidence["exe_sha256"]:
                        raise ValueError("Executable changed during the acceptance matrix")
                    stem = f"probe-{size_mib}mib-{repeat}"
                    raw_path = contained(root / f"{stem}.json", root)
                    stdout_path = contained(root / f"{stem}.stdout.log", root)
                    stderr_path = contained(root / f"{stem}.stderr.log", root)
                    if any(path.exists() for path in [raw_path, stdout_path, stderr_path]):
                        raise ValueError(f"Refusing to overwrite run evidence: {stem}")
                    command = [str(exe), "--file", str(fixture), "--record-bytes", str(RECORD_BYTES),
                               "--output", str(raw_path)]
                    run = {"size_mib": size_mib, "repeat": repeat, "command": command,
                           "stdout": stdout_path.name, "stderr": stderr_path.name, "raw_path": raw_path.name}
                    started = time.monotonic()
                    with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
                        try:
                            process = subprocess.run(command, stdout=stdout, stderr=stderr, check=False,
                                                     timeout=args.run_timeout_seconds)
                            run["exit_code"] = process.returncode
                        except subprocess.TimeoutExpired:
                            run["exit_code"] = None
                            run["failure"] = "Owned child exceeded the fixed process deadline and was killed/reaped."
                    run["elapsed_seconds"] = time.monotonic() - started
                    run["exe_sha256"] = digest(exe)
                    if run["exe_sha256"] != evidence["exe_sha256"]:
                        run["failure"] = "Executable changed while the owned probe process was running"
                        run["exit_code"] = 1
                    if raw_path.exists():
                        run["raw_sha256"] = digest(raw_path)
                        run["raw"] = json.loads(raw_path.read_text(encoding="utf-8"))
                        if run["raw"].get("file_bytes") != size:
                            run["failure"] = "Probe observed a different file extent"
                            run["exit_code"] = 1
                    evidence["runs"].append(run)
                    output.write_text(json.dumps(evidence, indent=2), encoding="utf-8")
            finally:
                # Delete only this exact newly created path after containment/type/size checks.
                target = contained(fixture, root)
                if not target.is_file() or target.stat().st_size != receipt["bytes"]:
                    raise ValueError(f"Fixture changed before cleanup; retained for inspection: {target}")
                if target.stat().st_nlink != 1 or digest(target) != receipt["sha256"]:
                    raise ValueError(f"Fixture content/ownership changed before cleanup; retained: {target}")
                target.unlink()
                receipt["removed_after_runs"] = True
        full = sorted(args.sizes_mib) == [10, 100, 1024]
        evidence["acceptance"] = evaluate(evidence["runs"], full)
        evidence["status"] = evidence["acceptance"]["status"]
    except Exception as error:
        evidence["status"] = "FAIL"
        evidence["failure"] = str(error)
    finally:
        evidence["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        output.write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    return 0 if evidence["status"] == "PASS" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"log_view_memory: {error}", file=sys.stderr)
        raise SystemExit(2)
