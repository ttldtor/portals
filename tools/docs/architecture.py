# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
"""Generate checked-in architecture exports using only the Python standard library.

Caller: documentation authors, from the repository root. Input: snapshot maps.json
and architecture_viewer.html. Output: deterministic SVG/HTML beside the manifest.
Errors: invalid data, absent sources or stale exports return 1; argparse returns 2.
No Git/network/browser invocation and no changes outside the snapshot directory.
"""

import argparse
import html
import json
from pathlib import Path
import re
import sys
import textwrap

ROOT = Path(__file__).resolve().parents[2]
DEFAULT = ROOT / "docs/architecture/snapshots/m1-968651a/maps.json"
COLORS = {"owner": "#1759a8", "borrow": "#7045a1", "flow": "#08796b",
          "error": "#a33b27", "status": "#705400"}


def source_url(data, source):
    return f"https://github.com/ttldtor/portals/blob/{source.get('revision', data['revision'])}/{source['path']}#L{source['line']}"


def validate(data):
    if data["schema"] != 1 or not data["maps"]:
        raise ValueError("expected schema 1 and at least one map")
    if not re.fullmatch(r"[a-f0-9]{40}", data["revision"]):
        raise ValueError("revision must be a full commit hash")
    map_ids = set()
    for diagram in data["maps"]:
        if diagram["id"] in map_ids or not re.fullmatch(r"[a-z][a-z0-9-]*", diagram["id"]):
            raise ValueError("duplicate or invalid map id")
        map_ids.add(diagram["id"])
        if not diagram["nodes"]:
            raise ValueError("map must contain nodes")
        ids = set()
        for node in diagram["nodes"]:
            if node["id"] in ids or not re.fullmatch(r"[a-z][a-z0-9-]*", node["id"]):
                raise ValueError("duplicate or invalid node id")
            ids.add(node["id"])
            if node["kind"] not in COLORS:
                raise ValueError("unknown node role")
            for lang in ("en", "ru"):
                if not node["title"][lang] or not node["detail"][lang]:
                    raise ValueError("missing translation")
            source = node["source"]
            if not re.fullmatch(r"[a-f0-9]{40}", source.get("revision", data["revision"])):
                raise ValueError("source revision must be a full commit hash")
            path = Path(source["path"])
            if path.is_absolute() or ".." in path.parts or path.parts[0] not in ("include", "src", "docs"):
                raise ValueError("source must be a public repository path")
            lines = (ROOT / path).read_text(encoding="utf-8").splitlines()
            if not 1 <= source["line"] <= len(lines):
                raise ValueError(f"invalid source line: {path}")
        for edge in diagram.get("edges", []):
            if edge["from"] not in ids or edge["to"] not in ids:
                raise ValueError("edge references a missing node")


def svg(data, diagram, lang):
    # Fixed card geometry and wrapped titles keep text away from routing gutters.
    width, card_width, card_height, gap, margin = 960, 440, 154, 36, 22
    rows = (len(diagram["nodes"]) + 1) // 2
    height = 100 + rows * (card_height + gap)
    esc = html.escape
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
           f'<title id="title">{esc(diagram["title"][lang])}</title>',
           f'<desc id="desc">{esc(data["status"][lang])}. {esc(diagram["intro"][lang])}</desc>',
           '<style>text{font-family:system-ui,Segoe UI,sans-serif} .paper{fill:#f5f7fc}.card{fill:#fff}.ink{fill:#17233a}.muted{fill:#42516a} a:focus rect{stroke-width:5} @media(prefers-color-scheme:dark){.paper{fill:#121b2b}.card{fill:#1c2a40}.ink{fill:#f2f5fc}.muted{fill:#d2dced}}</style>',
           '<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#7387a8"/></marker></defs>',
           f'<rect class="paper" width="{width}" height="{height}" rx="12"/>',
           f'<text class="ink" x="22" y="32" font-size="23" font-weight="700">{esc(diagram["title"][lang])}</text>',
           f'<text class="muted" x="22" y="58" font-size="14">{esc(data["id"])} · {esc(data["status"][lang])}</text>']
    def position(index):
        row = index // 2
        column = index % 2 if row % 2 == 0 else 1 - index % 2
        return margin + column * (card_width + gap), 80 + row * (card_height + gap)

    positions = {node["id"]: position(index) for index, node in enumerate(diagram["nodes"])}
    for edge in diagram.get("edges", []):
        x1, y1 = positions[edge["from"]]
        x2, y2 = positions[edge["to"]]
        if y1 == y2:
            start = (x1 + card_width, y1 + card_height / 2) if x2 > x1 else (x1, y1 + card_height / 2)
            end = (x2, y2 + card_height / 2) if x2 > x1 else (x2 + card_width, y2 + card_height / 2)
        else:
            start = (x1 + card_width / 2, y1 + card_height)
            end = (x2 + card_width / 2, y2)
        out.append(f'<path d="M {start[0]} {start[1]} L {end[0]} {end[1]}" fill="none" stroke="#7387a8" stroke-width="2" marker-end="url(#arrow)"><title>{esc(edge["label"][lang])}</title></path>')
    for index, node in enumerate(diagram["nodes"]):
        x, y = position(index)
        color = COLORS[node["kind"]]
        out.append(f'<a href="{esc(source_url(data, node["source"]))}" target="_blank" rel="noopener" data-node="{node["id"]}"><title>{esc(node["detail"][lang])}</title>')
        out.append(f'<rect class="card" x="{x}" y="{y}" width="{card_width}" height="{card_height}" rx="10" stroke="{color}" stroke-width="2"/>')
        out.append(f'<rect x="{x}" y="{y}" width="7" height="{card_height}" rx="3" fill="{color}"/>')
        for line_index, line in enumerate(textwrap.wrap(node["title"][lang], width=30)):
            out.append(f'<text class="ink" x="{x+20}" y="{y+30+line_index*24}" font-size="20" font-weight="650">{esc(line)}</text>')
        for line_index, line in enumerate(textwrap.wrap(node["label"], width=42)):
            out.append(f'<text class="muted" x="{x+20}" y="{y+88+line_index*19}" font-size="14">{esc(line)}</text>')
        out.append(f'<text class="muted" x="{x+20}" y="{y+133}" font-size="12">{index+1} · {esc(node["kind"])} · source ↗</text></a>')
    out.append('</svg>')
    return "\n".join(out) + "\n"


def products(data):
    result = {}
    for diagram in data["maps"]:
        for lang in ("en", "ru"):
            result[f'{diagram["id"]}-{lang}.svg'] = svg(data, diagram, lang)
    payload = dict(data)
    payload["svgs"] = result.copy()
    template = Path(__file__).with_name("architecture_viewer.html").read_text(encoding="utf-8")
    result["index.html"] = template.replace("/*SNAPSHOT_DATA*/", json.dumps(payload, ensure_ascii=False).replace("</", "<\\/"))
    return result


def main():
    parser = argparse.ArgumentParser(description="Generate offline architecture SVG/HTML exports (Python standard library only).",
        epilog="Results: SVG in EN/RU and self-contained index.html beside maps.json. Exit: 0 success; 1 invalid input, I/O failure or stale exports; 2 invalid arguments. Default writes replace generated exports; --check reads only. No network, SDK/runtime dependencies or browser installation. Help performs no workload or writes.")
    parser.add_argument("--source", type=Path, default=DEFAULT, help="snapshot maps.json; default: docs/architecture/snapshots/m1-968651a/maps.json relative to this repository")
    parser.add_argument("--check", action="store_true", help="validate sources and compare exact generated bytes without writing")
    args = parser.parse_args()
    try:
        data = json.loads(args.source.read_text(encoding="utf-8"))
        validate(data)
        outputs = products(data)
        for name, content in outputs.items():
            path = args.source.parent / name
            expected = content.encode("utf-8")
            if args.check:
                if not path.exists() or path.read_bytes() != expected:
                    raise ValueError(f"stale export: {path}")
            else:
                path.write_bytes(expected)
        print(f'{"Checked" if args.check else "Generated"} {len(outputs)} exports; {len(data["maps"])} maps; revision {data["revision"]}')
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"architecture: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
