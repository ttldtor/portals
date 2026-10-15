"""Select PR checks from a Git diff and validate the required CI result."""

import argparse
import json
import os
from pathlib import PurePosixPath
import subprocess


def select_mode(paths):
    """Only explicitly recognized documentation paths may skip C++ checks."""
    def is_documentation(path):
        file = PurePosixPath(path)
        if path in {"README.md", "ReleaseNotes.md", "docs/diagrams.css",
                    "docs/DoxygenLayout.xml", "docs/mermaid.json"}:
            return True
        if file.parts[:1] != ("docs",):
            return False
        return file.suffix == ".md" or (
            file.parts[:2] == ("docs", "images")
            and file.suffix in {".svg", ".png", ".jpg", ".jpeg", ".gif", ".webp"})

    return "docs" if paths and all(is_documentation(path) for path in paths) else "full"


def validate_results(needs):
    """Fail closed when selection or any selected check did not succeed."""
    changes = needs.get("changes", {})
    mode = changes.get("outputs", {}).get("mode")
    if changes.get("result") != "success" or mode not in {"docs", "full"}:
        raise ValueError("Check selection failed or returned an invalid mode")
    required = ("docs", "naming", "smoke") if mode == "full" else ("docs",)
    for job in required:
        result = needs.get(job, {}).get("result")
        if result != "success":
            raise ValueError(f"Required job {job}: {result!r}")
    for job in ("naming", "smoke"):
        if mode == "docs" and needs.get(job, {}).get("result") not in {"success", "skipped"}:
            raise ValueError(f"Unexpected result for {job}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    select = commands.add_parser("select", help="Choose docs/full from the complete base...head Git diff", description="Reads Git only. Prints JSON; appends mode/summary to GITHUB_OUTPUT/GITHUB_STEP_SUMMARY when set. Exit 0 on success, nonzero on Git or output failure.")
    select.add_argument("--base", required=True, help="Existing base Git revision; compare using its merge base with head")
    select.add_argument("--head", required=True, help="Existing candidate Git revision")
    commands.add_parser("gate", help="Validate CI_NEEDS JSON from completed jobs", description="Requires CI_NEEDS in the environment, containing the needs jobs/results JSON. Exit 0 only when all selected jobs succeeded; failures return nonzero. Does not run tests or change repository files.")
    args = parser.parse_args()
    if args.command == "gate":
        validate_results(json.loads(os.environ["CI_NEEDS"]))
        print("All selected checks succeeded")
        return

    # Disabling rename detection exposes both old and new paths. NUL separators
    # preserve spaces, newlines and non-ASCII names without quoting ambiguities.
    diff = subprocess.check_output([
        "git", "diff", "--name-only", "--no-renames", "-z",
        f"{args.base}...{args.head}", "--"])
    paths = [os.fsdecode(path) for path in diff.split(b"\0") if path]
    mode = select_mode(paths)
    reason = ("Every changed path is recognized documentation" if mode == "docs"
              else "Code, infrastructure, unknown paths, or an empty diff require full checks")
    print(json.dumps({"mode": mode, "reason": reason, "paths": paths}))
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.write(f"mode={mode}\n")
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
            summary.write(f"## Selected checks: {mode}\n\n{reason}.\n\nChanged paths: {len(paths)}.\n")


if __name__ == "__main__":
    main()
