# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
"""Measure observed Ninja dependencies and lexical public include relationships.

Run --help for the standalone authoring CLI. SDK use has no Python dependency.
This analyzer never builds, edits sources, selects CI jobs or invokes a compiler.
"""

import argparse
import hashlib
import json
from pathlib import Path
import posixpath
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
COMPILE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".c++", ".m", ".mm"}
MODULE_SUFFIXES = {".ixx", ".cppm", ".mpp"}
DEP_HEADER = re.compile(r"^(.*): #deps (\d+), deps mtime (-?\d+) \(([^)]+)\)$")


def canonical(value, base=""):
    """Normalize Ninja paths lexically on either host, preserving embedded spaces."""
    value = value.replace("\\", "/")
    base = base.replace("\\", "/")
    if not value.startswith("/") and not re.match(r"^[A-Za-z]:/", value):
        value = posixpath.join(base, value)
    value = posixpath.normpath(value)
    return value.casefold() if re.match(r"^[A-Za-z]:/|^//", value) else value


def relative(value, root):
    root = canonical(root).rstrip("/")
    return value[len(root) + 1:] if value.startswith(root + "/") else None


def parse_deps(text):
    """Return raw output records; malformed/count/duplicate records fail explicitly."""
    records = {}
    current = None
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        match = DEP_HEADER.fullmatch(line)
        if match:
            output, count, mtime, state = match.groups()
            if output in records:
                raise ValueError(f"duplicate dependency output at line {number}: {output}")
            current = {"output": output, "count": int(count), "mtime": int(mtime),
                       "state": state, "dependencies": []}
            records[output] = current
        elif line.startswith("    ") and current:
            current["dependencies"].append(line[4:])
        else:
            raise ValueError(f"malformed Ninja dependency line {number}")
    for record in records.values():
        if record["count"] != len(record["dependencies"]):
            raise ValueError(f"dependency count mismatch: {record['output']}")
    return list(records.values())


def parse_compdb(text):
    rows = json.loads(text)
    if not isinstance(rows, list):
        raise ValueError("compdb must be a JSON array")
    compiled, modules, noncompile = {}, [], set()
    for row in rows:
        if not isinstance(row, dict) or not all(isinstance(row.get(key), str) for key in ("directory", "file", "output")):
            raise ValueError("Ninja compdb rows require directory, file and output strings")
        source = canonical(row["file"], row["directory"])
        output = canonical(row["output"], row["directory"])
        suffix = posixpath.splitext(source)[1]
        if suffix in MODULE_SUFFIXES:
            modules.append(source)
        if suffix.lower() not in COMPILE_SUFFIXES or posixpath.splitext(output)[1].lower() not in {".o", ".obj"}:
            noncompile.add(output)
            continue
        if output in compiled:
            raise ValueError(f"duplicate compilation output: {output}")
        compiled[output] = {"source": source, "directory": canonical(row["directory"]),
                            "display_output": row["output"].replace("\\", "/")}
    if not compiled:
        raise ValueError("no supported compilation outputs in Ninja compdb")
    return compiled, sorted(set(modules)), noncompile


def public_includes(source_root):
    """Scan literal includes only; this is not the compiler's conditional include graph."""
    public_root = source_root / "include/portals"
    if not public_root.is_dir():
        raise ValueError("source root must contain include/portals")
    files = sorted(path for path in public_root.rglob("*") if path.suffix in {".h", ".hpp"})
    nodes = {path.relative_to(source_root).as_posix(): path for path in files}
    edges = set()
    unresolved, nonliteral = [], []
    fingerprints = {}
    for name, path in nodes.items():
        raw = path.read_bytes()
        fingerprints[name] = hashlib.sha256(raw).hexdigest()
        # Remove comments before finding directives; retain newlines for directive boundaries.
        text = re.sub(r"/\*.*?\*/|//[^\n]*", lambda m: "\n" * m[0].count("\n"), raw.decode("utf-8"), flags=re.S)
        for directive in re.finditer(r"^\s*#\s*include\s+([^\n]+)", text, re.M):
            literal = re.match(r'[<"]([^>"\n]+)[>"]', directive[1])
            if not literal:
                nonliteral.append(name)
                continue
            value = literal[1]
            if value.startswith("portals/"):
                target = "include/" + value
            elif directive[1].startswith('"'):
                target = canonical(value, posixpath.dirname(name))
            else:
                continue
            if target in nodes:
                edges.add((name, target))
            elif value.startswith("portals/"):
                unresolved.append({"from": name, "include": value})
    return {"kind": "lexical literal #include; includes inactive conditional branches; excludes macro and module imports",
            "nodes": sorted(nodes), "edges": [list(edge) for edge in sorted(edges)],
            "subsystems": sorted({posixpath.dirname(name) for name in nodes}),
            "unresolved_public_includes": unresolved, "nonliteral_include_headers": sorted(set(nonliteral)),
            "source_sha256": fingerprints}


def target_name(output):
    match = re.search(r"(?:^|/)CMakeFiles/([^/]+)\.dir/", output)
    return match[1] if match else None


def analyze(deps_text, compdb_text, source_root, recorded_root, changed=(), live=False):
    compiled, modules, noncompile = parse_compdb(compdb_text)
    records = parse_deps(deps_text)
    # A recorded compdb can use one build directory per configuration. Ambiguous
    # relative Ninja output roots cannot be guessed without inventing coverage.
    directories = {row["directory"] for row in compiled.values()}
    if len(directories) != 1:
        raise ValueError("expected one Ninja build directory; split multi-directory captures")
    build_root = next(iter(directories))
    dependencies = {}
    for record in records:
        output = canonical(record["output"], build_root)
        if output in dependencies:
            raise ValueError(f"duplicate normalized dependency output: {output}")
        dependencies[output] = record
    missing, stale, freshness_unknown = [], [], []
    objects, headers = [], {}
    stat_cache = {}

    def mtime(path):
        if path not in stat_cache:
            try:
                stat_cache[path] = Path(path).stat().st_mtime_ns
            except OSError:
                stat_cache[path] = None
        return stat_cache[path]

    for output, row in sorted(compiled.items()):
        object_path = relative(output, build_root) or output
        record = dependencies.get(output)
        if record is None:
            missing.append(object_path)
            continue
        paths = {canonical(value, build_root) for value in record["dependencies"]}
        paths.add(row["source"])
        reasons = []
        if record["state"] != "VALID" or record["count"] == 0:
            reasons.append("Ninja record is not VALID with dependencies")
        if live:
            output_time = mtime(output)
            if output_time is None:
                reasons.append("compiled output is absent")
            else:
                for path in sorted(paths):
                    source_time = mtime(path)
                    if source_time is None:
                        freshness_unknown.append(path)
                    elif source_time > output_time:
                        reasons.append("dependency is newer than compiled output: " + path)
        if reasons:
            stale.append({"output": object_path, "reasons": reasons})
        public = sorted(value for path in paths if (value := relative(path, recorded_root)) and value.startswith("include/portals/"))
        source = relative(row["source"], recorded_root) or row["source"]
        item = {"output": object_path, "source": source, "target": target_name(row["display_output"]),
                "subsystem": posixpath.dirname(source), "public_headers": public,
                "repository_dependencies": sorted(value for path in paths if (value := relative(path, recorded_root)) is not None),
                "valid_record": not reasons}
        objects.append(item)
        for header in public:
            headers.setdefault(header, []).append(object_path)
    noncompile_records = sorted(relative(output, build_root) or output for output in dependencies if output in noncompile)
    extra = sorted(relative(output, build_root) or output for output in dependencies if output not in compiled and output not in noncompile)
    coverage_complete = not (missing or stale or extra or modules)
    fresh = live and not freshness_unknown and coverage_complete
    changes = []
    known = {path for item in objects for path in item["repository_dependencies"]}
    for value in changed:
        normalized = canonical(value)
        if normalized.startswith("/") or re.match(r"^[a-z]:/", normalized):
            normalized = relative(normalized, str(source_root)) or relative(normalized, recorded_root) or normalized
        affected = [item for item in objects if normalized in item["repository_dependencies"]]
        recognized = normalized in known
        unknown_targets = [item["output"] for item in affected if item["target"] is None]
        changes.append({"path": normalized, "status": "observed" if recognized else "unknown",
                        "affected_objects": [item["output"] for item in affected] if recognized else None,
                        "affected_translation_units": sorted({item["source"] for item in affected}) if recognized else None,
                        "affected_targets": sorted({item["target"] for item in affected}) if recognized and not unknown_targets else None,
                        "unknown_target_objects": unknown_targets,
                        "recommendation": "review observed footprint; keep required checks" if recognized and fresh and not unknown_targets else "full/unknown; do not skip required checks"})
    graph = public_includes(source_root)
    directory_edges = sorted({(item["subsystem"], posixpath.dirname(header)) for item in objects for header in item["public_headers"]})
    return {"schema": 1, "scope": "configured supported compile rows only; unconfigured targets, future configurations, macro includes and module imports are not covered",
            "provenance": {"mode": "live Ninja read-only" if live else "recorded Ninja captures",
                           "recorded_source_root": recorded_root, "analysis_source_root": str(source_root),
                           "deps_sha256": hashlib.sha256(deps_text.encode("utf-8")).hexdigest(),
                           "compdb_sha256": hashlib.sha256(compdb_text.encode("utf-8")).hexdigest()},
            "coverage": {"configured_compile_objects": len(compiled), "observed_compile_objects": len(objects),
                         "valid_compile_objects": sum(item["valid_record"] for item in objects),
                         "filtered_noncompile_rows": len(noncompile), "noncompile_dependency_records": noncompile_records,
                         "missing_objects": missing, "stale_objects": stale,
                         "unmatched_dependency_records": extra, "unsupported_module_sources": modules,
                         "record_coverage_complete": coverage_complete,
                         "freshness": ("not checked for recorded captures" if not live else
                                       "stale observed objects" if stale else
                                       "unknown: unavailable dependency paths" if freshness_unknown else
                                       "observed records checked against current filesystem; uncovered objects remain unknown"),
                         "unavailable_dependency_paths": sorted(set(freshness_unknown))},
            "summary": {"observed_public_headers": len(headers), "lexical_public_headers": len(graph["nodes"]),
                        "literal_public_include_edges": len(graph["edges"]),
                        "observed_source_subsystems": sorted({item["subsystem"] for item in objects}),
                        "observed_targets": sorted({item["target"] for item in objects if item["target"]})},
            "headers": [{"path": header, "affected_objects": len(outputs), "objects": outputs}
                        for header, outputs in sorted(headers.items(), key=lambda item: (-len(item[1]), item[0]))],
            "objects": objects, "changes": changes, "public_include_graph": graph,
            "observed_directory_edges": [list(edge) for edge in directory_edges]}


def dot(edges, label, nodes=()):
    lines = ['digraph dependencies {', '  graph [label=' + json.dumps(label) + ', labelloc="t"];',
             '  rankdir=LR;', '  node [shape=box];']
    lines.extend("  " + json.dumps(node) + ";" for node in sorted(set(nodes)))
    lines.extend("  " + json.dumps(start) + " -> " + json.dumps(end) + ";" for start, end in sorted({tuple(edge) for edge in edges}))
    return "\n".join([*lines, "}", ""])


def write_report(report, directory, deps_text, compdb_text):
    # Exclusive directory creation preserves previous receipts and all input captures.
    directory.mkdir(parents=True, exist_ok=False)
    (directory / "impact.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    graph = report["public_include_graph"]
    (directory / "public-includes.dot").write_text(dot(graph["edges"], graph["kind"], graph["nodes"]), encoding="utf-8")
    edges = [(item["source"], header) for item in report["objects"] for header in item["public_headers"]]
    (directory / "observed-headers.dot").write_text(dot(edges, "Observed transitive translation-unit dependencies", [item["source"] for item in report["objects"]]), encoding="utf-8")
    (directory / "subsystems.dot").write_text(dot(report["observed_directory_edges"], "Observed directory groups; directory names are not architectural role declarations", report["summary"]["observed_source_subsystems"]), encoding="utf-8")
    coverage = report["coverage"]
    text = ["# Observed include / rebuild footprint", "", report["scope"], "",
            f"Compile records: {coverage['observed_compile_objects']}/{coverage['configured_compile_objects']}; valid: {coverage['valid_compile_objects']}.",
            f"Public headers observed: {report['summary']['observed_public_headers']}; literal include edges: {report['summary']['literal_public_include_edges']}.",
            f"Freshness: {coverage['freshness']}.", "", "Counts describe dependency footprint, not compile duration or measured speed.", "",
            "| Public header | Observed affected objects |", "| --- | ---: |"]
    text.extend(f"| `{item['path']}` | {item['affected_objects']} |" for item in report["headers"])
    (directory / "summary.md").write_text("\n".join(text) + "\n", encoding="utf-8")
    (directory / "ninja-deps.txt").write_bytes(deps_text.encode("utf-8"))
    (directory / "ninja-compdb.json").write_bytes(compdb_text.encode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        epilog="Inputs are read only. Live mode runs Ninja tools, never a build. JSON is printed to stdout; --output-dir creates seven files in a NEW directory and never overwrites existing results. Exit 0: analysis produced (possibly incomplete/unknown); 1: invalid captures, Ninja/I/O failure; 2: invalid arguments. --help needs no workload inputs and writes nothing. Python 3.10+ standard library only; Graphviz is optional for viewing DOT. This is evidence, not a CI check selector.")
    parser.add_argument("--source-root", type=Path, default=ROOT, help="checkout for the separately labelled lexical public include graph")
    parser.add_argument("--recorded-source-root", help="source root spelling in Ninja records; required offline; live default is --source-root")
    parser.add_argument("--source-revision", default="unknown", help="caller-declared source revision label; not verified by this analyzer")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--build-dir", type=Path, help="existing single-configuration Ninja build directory; runs -t deps and -t compdb read-only")
    mode.add_argument("--deps", type=Path, help="recorded ninja -t deps UTF-8 text; requires --compdb and --recorded-source-root")
    parser.add_argument("--compdb", type=Path, help="recorded ninja -t compdb JSON (must include output; ordinary compile_commands.json may not)")
    parser.add_argument("--ninja", default="ninja", help="existing Ninja executable for live mode; no installation is performed")
    parser.add_argument("--changed", action="append", default=[], help="changed source-relative or absolute file; repeatable; unknown impact is never zero")
    parser.add_argument("--output-dir", type=Path, help="new receipt directory for impact.json, three DOT graphs, summary.md and both raw captures; default stdout JSON only")
    args = parser.parse_args()
    if args.deps and (not args.compdb or not args.recorded_source_root):
        parser.error("--deps requires --compdb and --recorded-source-root")
    if args.build_dir and args.compdb:
        parser.error("--compdb is only valid with --deps")
    if args.recorded_source_root:
        recorded = canonical(args.recorded_source_root)
        if not recorded.startswith("/") and not re.match(r"^[a-z]:/", recorded):
            parser.error("--recorded-source-root must be an absolute Windows or POSIX path")
    try:
        if args.build_dir:
            captures = []
            for tool in ("deps", "compdb"):
                result = subprocess.run([args.ninja, "-t", tool], cwd=args.build_dir, capture_output=True,
                                        timeout=60, check=True)
                captures.append(result.stdout.decode("utf-8"))
            deps_text, compdb_text = captures
        else:
            deps_text = args.deps.read_bytes().decode("utf-8")
            compdb_text = args.compdb.read_bytes().decode("utf-8")
        root = args.source_root.resolve()
        report = analyze(deps_text, compdb_text, root, args.recorded_source_root or str(root), args.changed, bool(args.build_dir))
        report["provenance"]["declared_source_revision"] = args.source_revision
        if args.output_dir:
            write_report(report, args.output_dir, deps_text, compdb_text)
        print(json.dumps(report, indent=2, ensure_ascii=False))
        return 0
    except subprocess.CalledProcessError as error:
        detail = error.stderr.decode("utf-8", errors="replace") if isinstance(error.stderr, bytes) else error.stderr
        print(f"change-impact: {error}\n{detail or ''}", file=sys.stderr)
        return 1
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"change-impact: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
