#!/usr/bin/env python3
"""Discard cached Debian archives not authenticated by current APT metadata.

CI runs this after apt-get update and before installation. The directory contains
only disposable download-cache entries; installed packages are never modified.
"""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote


def trusted_hashes(archive):
    """Resolve an archive filename against metadata from signed APT indices."""
    parts = archive.stem.split("_")
    if len(parts) != 3:
        return set()
    package, version, architecture = map(unquote, parts)
    if not re.fullmatch(r"[a-z0-9][a-z0-9+.-]*", package):
        return set()
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", architecture):
        return set()
    result = subprocess.run(
        ["apt-cache", "show", f"{package}:{architecture}={version}"],
        capture_output=True, text=True, check=False,
    )
    if result.returncode:
        return set()
    hashes = set()
    for paragraph in result.stdout.split("\n\n"):
        fields = dict(line.split(": ", 1) for line in paragraph.splitlines()
                      if not line.startswith(" ") and ": " in line)
        digest = fields.get("SHA256", "")
        if (fields.get("Package") == package and fields.get("Version") == version
                and fields.get("Architecture") == architecture
                and re.fullmatch(r"[0-9a-f]{64}", digest)):
            hashes.add(digest)
    return hashes


def prune(directory):
    """Keep only regular archives matching the current trusted SHA256."""
    kept = removed = 0
    for archive in sorted(directory.glob("*.deb")):
        valid = False
        if not archive.is_symlink() and archive.is_file():
            expected = trusted_hashes(archive)
            if expected:
                with archive.open("rb") as stream:
                    digest = hashlib.sha256()
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        digest.update(chunk)
                    valid = digest.hexdigest() in expected
        if valid:
            kept += 1
        else:
            archive.unlink()
            removed += 1
    return kept, removed


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Requires apt-cache and fresh signed APT indices. Deletes invalid *.deb files "
               "in DIRECTORY only. Exit 0: scan completed; 1: I/O/tool failure; 2: invalid arguments. "
               "Missing/obsolete metadata is a cache miss, not permission to install an archive.",
    )
    parser.add_argument("directory", type=Path, help="Disposable APT archive cache directory (required)")
    args = parser.parse_args()
    if not args.directory.is_dir():
        parser.error("DIRECTORY must already exist")
    try:
        kept, removed = prune(args.directory)
    except OSError as error:
        parser.exit(1, f"APT cache validation failed: {error}\n")
    print(f"APT cache: {kept} verified archives, {removed} discarded archives")


if __name__ == "__main__":
    main()
