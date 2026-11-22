"""Generate deterministic Viewer qualification fixtures without file-sized allocations.

Writes one create-exclusive synthetic SQL/Markdown file and its JSON manifest.
No user input files are read, no OS caches/settings are changed. Exit 0 succeeds,
2 rejects arguments, 1 reports I/O/generation failure and removes this run's partial
files. --help needs no destination and performs no generation.
"""
from __future__ import annotations
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import sys
import time

MIB = 1024 * 1024
RECORD_BYTES = 256
WRITE_BYTES = MIB
RESERVE_BYTES = 64 * MIB
SQL = "INSERT INTO events VALUES (42, 'Práha 界'); -- chunk 00000000 bounded source".encode("utf-8")
MARKDOWN = "# Document00000000\ntext *bold* `code`\n```sql\nSELECT 'Práha 界';\n```\n".encode("utf-8")


def record(kind: str) -> bytes:
    body = SQL if kind == "sql" else MARKDOWN
    return body + b" " * (RECORD_BYTES - len(body) - 1) + b"\n"


def chunks(kind: str, size: int, long_line: int):
    if long_line:
        prefix, suffix = (b"INSERT INTO events VALUES ('", b"');\n") if kind == "sql" else (b"# ", b"\n")
        yield prefix
        remaining = long_line - len(prefix) - len(suffix)
        block = b"x" * WRITE_BYTES
        while remaining:
            amount = min(remaining, len(block))
            yield block[:amount]
            remaining -= amount
        yield suffix
    remaining = size - long_line
    block = record(kind) * (WRITE_BYTES // RECORD_BYTES)
    block_index = 0
    while remaining:
        amount = min(remaining, len(block))
        tagged = block.replace(b"00000000", f"{block_index:08d}".encode("ascii"))
        yield tagged[:amount]
        remaining -= amount
        block_index += 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, required=True, help="New output file; its parent must already exist; never overwritten")
    parser.add_argument("--size-mib", type=int, default=16, help="Logical size in MiB, 1..8192 (default: 16)")
    parser.add_argument("--format", choices=("sql", "markdown"), default="sql", help="Repeated annotated 256-byte unit (default: sql)")
    parser.add_argument("--long-line-mib", type=int, default=0, help="Optional first line length in MiB, including newline; 0 disables (default: 0)")
    parser.add_argument("--gzip", action="store_true", help="Write gzip instead of raw bytes; fixed level1/mtime0, logical size unchanged")
    args = parser.parse_args(argv)
    if not 1 <= args.size_mib <= 8192 or not 0 <= args.long_line_mib <= args.size_mib:
        parser.error("require 1 <= size-mib <= 8192 and 0 <= long-line-mib <= size-mib")
    output = args.output.absolute()
    manifest = output.with_name(output.name + ".manifest.json")
    if not output.parent.is_dir() or output.exists() or manifest.exists():
        parser.error("require an existing parent and absent output/manifest")
    size = args.size_mib * MIB
    if shutil.disk_usage(output.parent).free < size + RESERVE_BYTES:
        parser.error("insufficient free space for conservative logical-size admission plus 64 MiB reserve")
    created = False
    manifest_created = False
    start = time.perf_counter()
    try:
        digest = hashlib.sha256()
        written = 0
        with output.open("xb") as raw:
            created = True
            destination = gzip.GzipFile(filename="", fileobj=raw, mode="wb", compresslevel=1, mtime=0) if args.gzip else raw
            try:
                for chunk in chunks(args.format, size, args.long_line_mib * MIB):
                    destination.write(chunk)
                    digest.update(chunk)
                    written += len(chunk)
            finally:
                if args.gzip:
                    destination.close()
        if written != size:
            raise RuntimeError("logical extent mismatch")
        physical = hashlib.sha256()
        with output.open("rb") as source:
            while chunk := source.read(WRITE_BYTES):
                physical.update(chunk)
        data = dict(schema="portals-document-corpus-1", path=str(output), format=args.format,
                    compressed=args.gzip, logical_bytes=written, physical_bytes=output.stat().st_size,
                    logical_sha256=digest.hexdigest(), physical_sha256=physical.hexdigest(),
                    record_bytes=RECORD_BYTES, record_hex=record(args.format).hex(),
                    block_marker="Each MiB has its eight-digit decimal block index in every repeated record",
                    long_line_bytes=args.long_line_mib * MIB,
                    generation_seconds=time.perf_counter() - start,
                    generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    memory_contract="At most bounded write/reader buffers; no complete logical file in memory")
        with manifest.open("x", encoding="utf-8") as stream:
            manifest_created = True
            json.dump(data, stream, indent=2)
            stream.write("\n")
        print(json.dumps(data))
        return 0
    except (OSError, RuntimeError) as error:
        if created:
            output.unlink(missing_ok=True)
        if manifest_created:
            manifest.unlink(missing_ok=True)
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
