# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
"""Plan local contract checks without executing builds/tests or changing hosted CI.

Run --help for standalone usage. Exact audited contracts live in
check_contracts.json. A dependency graph does not establish semantic test
coverage: this pilot never excludes C++ tests from a full runtime plan.
"""

import sys

sys.dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re

from checks import select_mode

ROOT = Path(__file__).resolve().parents[2]
MATRIX = Path(__file__).with_name("check_contracts.json")


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False).encode("utf-8")).hexdigest()


def validate_changes(changes):
    if not isinstance(changes, list):
        raise ValueError("changes must be an array of {path,status} records")
    for item in changes:
        if not isinstance(item, dict) or not isinstance(item.get("path"), str) or not isinstance(item.get("status"), str):
            raise ValueError("each change requires string path and status")
        path = PurePosixPath(item["path"])
        if path.is_absolute() or ".." in path.parts or "\\" in item["path"] or ":" in item["path"] or not path.parts or path.as_posix() != item["path"]:
            raise ValueError("change paths must be repository-relative POSIX paths")
    if len({item["path"] for item in changes}) != len(changes):
        raise ValueError("duplicate changed path")


def inventory_names(inventory):
    if inventory is None:
        return None
    if not isinstance(inventory, dict) or inventory.get("kind") != "ctestInfo" or not isinstance(inventory.get("tests"), list):
        raise ValueError("inventory must be ctest --show-only=json-v1 output")
    if any(not isinstance(item, dict) or not isinstance(item.get("name"), str) or not item["name"] for item in inventory["tests"]):
        raise ValueError("inventory tests require nonempty string names")
    tests = [item["name"] for item in inventory["tests"]]
    if len(set(tests)) != len(tests):
        raise ValueError("inventory has invalid or duplicate test names")
    return sorted(tests)


def dependency_audit(impact, changes):
    if impact is None:
        return {"status": "missing", "issues": ["no dependency receipt supplied"]}
    if not isinstance(impact, dict) or impact.get("schema") != 1 or not isinstance(impact.get("coverage"), dict) or not isinstance(impact.get("objects"), list):
        raise ValueError("impact must be a PRTLS-8 schema1 receipt")
    if not isinstance(impact.get("provenance"), dict) or not isinstance(impact["provenance"].get("mode"), str):
        raise ValueError("impact requires a provenance object with a string mode")
    coverage = impact["coverage"]
    if any(not isinstance(item, dict) or not isinstance(item.get("repository_dependencies"), list)
           or not all(isinstance(path, str) for path in item["repository_dependencies"]) for item in impact["objects"]):
        raise ValueError("impact objects require repository dependency paths")
    for key in ("missing_objects", "unmatched_dependency_records", "unsupported_module_sources", "unavailable_dependency_paths"):
        if key in coverage and (not isinstance(coverage[key], list) or not all(isinstance(path, str) for path in coverage[key])):
            raise ValueError("impact coverage path fields must be arrays of strings")
    if "stale_objects" in coverage and (not isinstance(coverage["stale_objects"], list) or
                                        any(not isinstance(item, dict) or not isinstance(item.get("output"), str) for item in coverage["stale_objects"])):
        raise ValueError("impact stale objects require output records")
    issues = []
    if coverage.get("record_coverage_complete") is not True:
        issues.append("compile dependency coverage is incomplete")
    for key in ("missing_objects", "stale_objects", "unmatched_dependency_records", "unsupported_module_sources", "unavailable_dependency_paths"):
        if coverage.get(key):
            issues.append(key)
    if impact.get("provenance", {}).get("mode") != "live Ninja read-only":
        issues.append("recorded dependencies have no current freshness proof")
    known = {path for item in impact["objects"] for path in item.get("repository_dependencies", [])}
    for change in changes:
        if change["path"] not in known:
            issues.append("unobserved changed file: " + change["path"])
    return {"status": "unqualified" if issues else "observed only; semantic exclusions not qualified",
            "issues": issues, "receipt_sha256": digest(impact),
            "observed_compile_objects": coverage.get("observed_compile_objects"),
            "configured_compile_objects": coverage.get("configured_compile_objects")}


def plan(changes, inventory, matrix, source_root, level="task", impact=None):
    validate_changes(changes)
    names = inventory_names(inventory)
    if not isinstance(matrix, dict) or matrix.get("schema") != 1 or not isinstance(matrix.get("contracts"), dict):
        raise ValueError("unsupported contract matrix")
    for field in ("integration", "milestone_additions"):
        if not isinstance(matrix.get(field), list) or not all(isinstance(value, str) for value in matrix[field]):
            raise ValueError("matrix obligations must be arrays of strings")
    for contract in matrix["contracts"].values():
        if not isinstance(contract, dict) or not isinstance(contract.get("kind"), str) or contract["kind"] not in {"python", "cxx"} or not contract.get("tests"):
            raise ValueError("contract requires a supported kind and named tests")
        for field in ("paths", "tests", "witnesses", "cross"):
            if not isinstance(contract.get(field), list) or not all(isinstance(value, str) for value in contract[field]):
                raise ValueError("contract fields must be arrays of strings")
        if not contract["paths"] or not contract["witnesses"]:
            raise ValueError("contracts require paths and source witnesses")
        validate_changes([{"path": path, "status": "M"} for path in sorted(set(contract["paths"] + contract["witnesses"]))])
    paths = [item["path"] for item in changes]
    hosted = select_mode(paths)
    reasons = []
    contracts = matrix["contracts"]
    direct = {name for name, contract in contracts.items() if set(paths) & set(contract["paths"])}
    selected = set(direct)
    pending = list(direct)
    while pending:
        for cross in contracts[pending.pop()]["cross"]:
            if cross not in contracts:
                raise ValueError("unknown cross-contract: " + cross)
            if cross not in selected:
                selected.add(cross)
                pending.append(cross)
    witnesses = sorted({path for name in selected for path in contracts[name]["witnesses"]})
    missing_witnesses = [path for path in witnesses if not (source_root / path).is_file()]
    focus = sorted({test for name in selected for test in contracts[name]["tests"]})
    missing_tests = sorted(set(focus) - set(names)) if names is not None else focus
    if not changes:
        reasons.append("empty change set")
    if any(item["status"] != "M" for item in changes):
        reasons.append("added/deleted/renamed/copied or unknown change status requires full")
    if any(not (source_root / path).is_file() for path in paths):
        reasons.append("a changed path is absent from this source checkout")
    if names is None or not names:
        reasons.append("configured CTest inventory is missing or empty")
    if missing_witnesses:
        reasons.append("contract source witnesses are absent")
    if missing_tests:
        reasons.append("contract CTest registrations are absent")
    covered_paths = {path for name in direct for path in contracts[name]["paths"]}
    documentation = hosted == "docs"
    if not documentation and set(paths) - covered_paths:
        reasons.append("unmapped changed paths")
    runtime = any(contracts[name]["kind"] == "cxx" for name in selected)
    provided_audit = dependency_audit(impact, changes if runtime else []) if impact is not None else None
    audit = (provided_audit or dependency_audit(None, changes)) if runtime else {
        "status": "not applicable to this non-C++ preflight", "issues": [], "provided_receipt": provided_audit}
    if runtime:
        reasons.extend(audit["issues"])
        reasons.append("runtime semantic exclusions are not qualified by this bounded pilot")
    if level != "task":
        reasons.append(level + " requires the full configured suite")
    mode = "full" if reasons else "documentation" if documentation else "focused preflight"
    chosen = names if mode == "full" else [] if mode == "documentation" else focus
    regex = "^(" + "|".join(re.escape(name) for name in chosen) + ")$" if mode == "focused preflight" else None
    witness_hashes = {path: hashlib.sha256((source_root / path).read_bytes()).hexdigest() for path in witnesses if path not in missing_witnesses}
    return {"schema": 1, "level": level, "mode": mode, "changes": changes,
            "focus_contracts": sorted(selected), "direct_contracts": sorted(direct), "focus_tests": focus,
            "selected_tests": chosen, "ctest_regex": regex,
            "ctest_arguments": None if mode == "documentation" else ["--output-on-failure"] + (["-R", regex] if regex else []),
            "proof": {"policy_sha256": digest(matrix), "inventory_sha256": digest(inventory) if inventory is not None else None,
                      "configured_tests": len(names) if names is not None else None,
                      "selected_tests": len(chosen) if chosen is not None else None,
                      "contract_witnesses": witnesses, "witness_sha256": witness_hashes, "missing_witnesses": missing_witnesses,
                      "missing_ctest_names": missing_tests, "fallback_reasons": sorted(set(reasons)), "dependency_audit": audit,
                      "coverage_claim": "all configured tests selected" if mode == "full" and names is not None else
                                        "explicit non-C++ local fixtures only" if mode == "focused preflight" else
                                        "no runtime exclusion claim"},
            "hosted_ci": {"existing_selector_mode": hosted, "policy_changed": False,
                          "required_jobs": ["changes", "docs", "CI result"] + (["naming", "smoke (Windows MSVC / Linux GCC)"] if hosted == "full" else [])},
            "persistent_obligations": {"integration": matrix["integration"], "milestone_additions": matrix["milestone_additions"]},
            "execution": "plan only; caller selects build directory, compiler and configuration; no tests/builds executed"}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        epilog="Reads inputs only and prints JSON. Optional --output creates a NEW file, never overwrites. Exit0: plan produced, including full fallback;1: invalid input/I/O;2: invalid CLI. Help requires no workload inputs or writes. No Git, compiler, CTest invocation, network or SDK runtime dependency. Integration/milestone obligations remain mandatory.")
    parser.add_argument("--changes", type=Path, required=True, help="UTF-8 JSON array of repository-relative POSIX path/status records; only M qualifies for focused task preflight; include both rename paths")
    parser.add_argument("--inventory", type=Path, help="captured ctest --show-only=json-v1; absent means full/unknown configured test set")
    parser.add_argument("--impact", type=Path, help="optional PRTLS-8 impact.json; records coverage limitations, never proves runtime exclusions")
    parser.add_argument("--source-root", type=Path, default=ROOT, help="source checkout for exact contract/witness existence checks")
    parser.add_argument("--level", choices=("task", "integration", "milestone"), default="task", help="verification level; integration/milestone always retain full configured suite")
    parser.add_argument("--output", type=Path, help="new UTF-8 JSON output file; parent must exist; default stdout only")
    args = parser.parse_args()
    try:
        read = lambda path: json.loads(path.read_text(encoding="utf-8-sig")) if path else None
        result = plan(read(args.changes), read(args.inventory), read(MATRIX), args.source_root.resolve(), args.level, read(args.impact))
        text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as output:
                output.write(text)
        print(text, end="")
        return 0
    except (OSError, ValueError, TypeError, KeyError) as error:
        print(f"check-plan: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
