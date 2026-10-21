"""Internal diagram variants, called by language_site.finalize_languages.

Inputs: generated HTML root, repository root and locale-to-Doxygen-tag mapping.
prepare_diagrams writes public Turbo/Turbo2 SVGs; its render inputs, cache and
inventory stay in a build sibling .theme-diagrams-cache. rewrite_diagrams
returns page markup retaining the original asset and accessible description.
Mermaid uses the installed mmdc/Node runtime already required by the docs build.
Missing tools, invalid SVG/JSON, render failures and I/O errors stop the build;
source diagrams, screenshots and frozen architecture exports are never changed.
"""

from dataclasses import dataclass, field
import hashlib
import html
import json
from pathlib import Path
import posixpath
import re
import shutil
import subprocess
import textwrap
from urllib.parse import unquote, urlsplit
import xml.etree.ElementTree as ET


SVG = "{http://www.w3.org/2000/svg}"
FONT = '"Cascadia Mono","DejaVu Sans Mono",Consolas,"Liberation Mono",monospace'
PALETTES = {
    "turbo": {"background": "#000080", "panel": "#00005c", "ink": "#ffffdf",
              "border": "#c0c0c0", "accent": "#55ffff", "title": "#ffff55"},
    "turbo2": {"background": "#1616aa", "panel": "#1616aa", "ink": "#ffffdf",
               "border": "#b0b0b0", "accent": "#00ffff", "title": "#ffff55"},
}


@dataclass
class DiagramCatalog:
    root: Path
    assets: dict = field(default_factory=dict)
    inventory: list = field(default_factory=list)


def _dimensions(svg):
    element = ET.fromstring(svg)
    box = element.get("viewBox", "").split()
    if len(box) != 4:
        raise ValueError("diagram SVG must have a viewBox")
    return float(box[2]), float(box[3])


def _snapshot_svg(data, diagram, language, palette):
    """Render the manifest's nodes, source links and edges at a fixed 16px size."""
    esc = html.escape
    margin, card_width, column_gap, line_height = 16, 296, 24, 24
    width = margin * 2 + card_width * 2 + column_gap
    title_lines = textwrap.wrap(diagram["title"][language], 61)
    status_lines = textwrap.wrap(data["id"] + " · " + data["status"][language], 61)
    y = margin + line_height * (len(title_lines) + len(status_lines)) + 24
    positions, rows = {}, []
    nodes = diagram["nodes"]
    for row_start in range(0, len(nodes), 2):
        pair = nodes[row_start:row_start + 2]
        records = []
        for offset, node in enumerate(pair):
            title = textwrap.wrap(node["title"][language], 26)
            label = textwrap.wrap(node["label"], 26, break_long_words=True)
            height = 24 + line_height * (len(title) + len(label) + 2)
            column = offset if row_start // 2 % 2 == 0 else 1 - offset
            x = margin + column * (card_width + column_gap)
            records.append((node, x, title, label, height))
        row_height = max(record[-1] for record in records)
        for node, x, title, label, _ in records:
            positions[node["id"]] = (x, y, card_width, row_height)
            rows.append((node, x, y, title, label, row_height))
        y += row_height + 48
    height = y - 24
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
           f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
           f'<title id="title">{esc(diagram["title"][language])}</title>',
           f'<desc id="desc">{esc(data["status"][language])}. {esc(diagram["intro"][language])}</desc>',
           f'<metadata>{esc(json.dumps(diagram, ensure_ascii=False))}</metadata>',
           f'<style>text{{font-family:{FONT};font-size:16px;fill:{palette["ink"]}}}'
           f'.heading{{font-weight:bold;fill:{palette["title"]}}}'
           f'a:focus rect{{stroke:{palette["title"]};stroke-width:3}}</style>',
           '<defs><marker id="arrow" viewBox="0 0 8 8" refX="7" refY="4" '
           'markerWidth="8" markerHeight="8" orient="auto" markerUnits="userSpaceOnUse">'
           f'<path d="M0 0L8 4L0 8Z" fill="{palette["accent"]}"/></marker></defs>',
           f'<rect width="{width}" height="{height}" fill="{palette["background"]}"/>']
    for index, line in enumerate(title_lines + status_lines):
        out.append(f'<text x="20" y="{36 + index * line_height}" '
                   f'class="{"heading" if index < len(title_lines) else "status"}">{esc(line)}</text>')
    for edge in diagram.get("edges", []):
        x1, y1, w1, h1 = positions[edge["from"]]
        x2, y2, _, _ = positions[edge["to"]]
        if y1 == y2:
            start = (x1 + w1, y1 + h1 / 2) if x2 > x1 else (x1, y1 + h1 / 2)
            end = (x2, y2 + h1 / 2) if x2 > x1 else (x2 + w1, y2 + h1 / 2)
            route = f'M{start[0]} {start[1]}H{end[0]}'
        else:
            start, end = (x1 + w1 / 2, y1 + h1), (x2 + w1 / 2, y2)
            middle = (start[1] + end[1]) / 2
            route = f'M{start[0]} {start[1]}V{middle}H{end[0]}V{end[1]}'
        out.append(f'<path data-from="{edge["from"]}" data-to="{edge["to"]}" '
                   f'd="{route}" fill="none" stroke="{palette["accent"]}" stroke-width="2" '
                   f'marker-end="url(#arrow)"><title>{esc(edge["label"][language])}</title></path>')
    for index, (node, x, y, title, label, height) in enumerate(rows):
        source = node["source"]
        revision = source.get("revision", data["revision"])
        link = f'https://github.com/ttldtor/portals/blob/{revision}/{source["path"]}#L{source["line"]}'
        accessible_label = node["title"][language] + ": " + node["label"]
        out.extend([f'<a href="{esc(link, quote=True)}" target="_blank" rel="noopener" data-node="{node["id"]}" '
                    f'aria-label="{esc(accessible_label, quote=True)}">',
                    f'<title>{esc(node["detail"][language])}</title>',
                    f'<rect x="{x}" y="{y}" width="{card_width}" height="{height}" '
                    f'fill="{palette["panel"]}" stroke="{palette["border"]}" stroke-width="2"/>'])
        for line_index, line in enumerate(title + label):
            out.append(f'<text x="{x + 16}" y="{y + 28 + line_index * line_height}" '
                       f'class="{"heading" if line_index < len(title) else "label"}">{esc(line)}</text>')
        source_label = "источник" if language == "ru" else "source"
        out.append(f'<text x="{x + 16}" y="{y + height - 20}">{index + 1} · '
                   f'{esc(node["kind"])} · {source_label} ↗</text></a>')
    return "\n".join(out + ["</svg>"])


def _mermaid_command():
    executable = shutil.which("mmdc")
    if not executable:
        raise RuntimeError("Turbo diagram rendering requires the documentation mmdc executable")
    # npm's Windows installation also contains a Unix shell shim named mmdc.
    cli = Path(executable).parent / "node_modules/@mermaid-js/mermaid-cli/src/cli.js"
    if cli.is_file():
        node = shutil.which("node")
        if not node:
            raise RuntimeError("Turbo diagram rendering requires Node.js")
        return [node, str(cli)]
    return [executable]


def _render_mermaid(source, output, palette, cache_directory=None):
    text = source.read_text(encoding="utf-8")
    caption_width = 22 if "accTitle: Managed control ownership" in text else 28
    text = _wrap_mermaid_labels(text, caption_width)
    # Explicit source classes keep their identities; presentation comes from the theme.
    text = re.sub(r"^\s*classDef\s+(\S+)\s+.*$", lambda m:
                  f'  classDef {m[1]} fill:{palette["panel"]},stroke:{palette["border"]},color:{palette["ink"]}',
                  text, flags=re.M)
    # Rank parallel ownership branches vertically. Mermaid's invisible links
    # affect placement only; all visible nodes, labels and arrows are retained.
    ownership_columns = (("tree", "services", "scene"),
                         ("root", "registrations", "properties", "models"))
    if "accTitle: Managed control ownership" in text:
        defined = set(re.findall(r'\b(\w+)\s*[\[{]', text))
        for column in ownership_columns:
            if set(column) <= defined:
                text += "\n  " + " ~~~ ".join(column) + "\n"
    cache_directory = Path(cache_directory) if cache_directory else output.parent
    cache_directory.mkdir(parents=True, exist_ok=True)
    prepared = cache_directory / output.with_suffix(".mmd").name
    config_path = cache_directory / output.with_suffix(".json").name
    prepared.write_text(text, encoding="utf-8")
    config = {"theme": "base", "look": "classic", "fontFamily": FONT, "fontSize": 16, "htmlLabels": False,
              "themeVariables": {"fontFamily": FONT, "fontSize": "16px",
                  "primaryColor": palette["panel"], "primaryTextColor": palette["ink"],
                  "primaryBorderColor": palette["border"], "lineColor": palette["accent"],
                  "secondaryColor": palette["background"], "tertiaryColor": palette["background"],
                  "edgeLabelBackground": palette["background"]},
              "flowchart": {"useMaxWidth": False, "nodeSpacing": 24, "rankSpacing": 24,
                            "padding": 8, "minNodeWidth": 0, "wrappingWidth": 300,
                            "curve": "stepAfter", "htmlLabels": False}}
    config_path.write_text(json.dumps(config), encoding="utf-8")
    command = _mermaid_command() + ["-i", str(prepared), "-o", str(output),
                                   "-c", str(config_path), "-b", palette["background"]]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120, check=False)
    if result.returncode:
        raise RuntimeError(f'Mermaid rendering failed for {source}: {result.stdout}\n{result.stderr}')
    rendered = output.read_text(encoding="utf-8")
    if _dimensions(rendered)[0] > 680 and re.search(r'^(flowchart|graph)\s+LR\b', text, re.M):
        # Preserve compact horizontal chains; rank wider flows vertically.
        text = re.sub(r'^(flowchart|graph)\s+LR\b', r'\1 TB', text, flags=re.M)
        prepared.write_text(text, encoding="utf-8")
        result = subprocess.run(command, capture_output=True, text=True, timeout=120, check=False)
        if result.returncode:
            raise RuntimeError(f'Mermaid rendering failed for {source}: {result.stdout}\n{result.stderr}')
    # The external image receives its intrinsic pixel dimensions, never 100%.
    rendered = output.read_text(encoding="utf-8")
    element = ET.fromstring(rendered)
    width, height = _dimensions(rendered)
    element.set("width", str(width))
    element.set("height", str(height))
    element.set("style", f'background-color:{palette["background"]}')
    ET.register_namespace("", SVG[1:-1])
    output.write_text(ET.tostring(element, encoding="unicode"), encoding="utf-8")
    return {"source": text, "command_output": result.stdout + result.stderr}


def _wrap_mermaid_labels(source, width=28):
    """Wrap node captions while retaining identifiers, words and edge syntax."""
    def wrap(match):
        identity, opening, label, closing = match.groups()
        if (opening, closing) not in {('[', ']'), ('{', '}')} or '`' in label:
            return match[0]
        label = label.strip('"')
        lines = []
        for line in re.split(r'<br\s*/?>', label):
            lines.extend(textwrap.wrap(line, width, break_long_words=False, break_on_hyphens=False) or [''])
        return identity + opening + '"' + '<br/>'.join(lines) + '"' + closing
    return re.sub(r'\b([A-Za-z_]\w*)([\[{])("?[^\]\}\n]+"?)([\]}])', wrap, source)


def _attributes(markup):
    return {key: html.unescape(value) for key, value in re.findall(r'([\w-]+)="([^"]*)"', markup)}


def _external_bases(content, page, name):
    """Recover undocumented external bases omitted by Doxygen tag exports."""
    source_link = re.search(r'href="([^"]+_source\.html)"', content)
    if not source_link:
        raise ValueError(f'missing source listing for external inheritance in {page}')
    listing = (page.parent / html.unescape(source_link[1])).read_text(encoding='utf-8')
    listing = re.sub(r'<span class="lineno">.*?</span>', '', listing, flags=re.S)
    source = html.unescape(re.sub(r'<[^>]*>', '', listing))
    head, separator, arguments = name.partition('<')
    unqualified = head.rsplit('::', 1)[-1] + separator + arguments
    normalized = ''.join(unqualified.split())
    matches = []
    for declaration in re.finditer(r'\b(?:class|struct)\s+([A-Za-z_]\w*(?:\s*<[^{};]*>)?)\s*:(?!:)\s*([^{};]+)\{', source):
        if ''.join(declaration[1].split()) == normalized:
            bases, start, depth = [], 0, 0
            for index, character in enumerate(declaration[2]):
                depth += (character == '<') - (character == '>')
                if character == ',' and depth == 0:
                    bases.append(declaration[2][start:index])
                    start = index + 1
            bases.append(declaration[2][start:])
            matches.append([re.sub(r'\b(?:public|protected|private|virtual)\s+', '', base).strip()
                            for base in bases])
    if len(matches) != 1:
        raise ValueError(f'expected one source declaration for external inheritance {name} in {page}; found {len(matches)}')
    return matches[0]


def _inheritance_graphs(root, tag_files):
    """Pair Doxygen image maps with tag-file base relations, never raster pixels."""
    compounds, names = {}, {}
    for tag_file in (tag_files or {}).values():
        for compound in ET.parse(tag_file).getroot().findall('compound'):
            if compound.get('kind') not in {'class', 'struct'}:
                continue
            filename, name = compound.findtext('filename'), compound.findtext('name')
            if not filename or not name:
                continue
            compounds[filename] = compound
            normalized = ''.join(name.split())
            if normalized in names and names[normalized] != filename:
                raise ValueError(f'ambiguous inheritance type: {name}')
            names[normalized] = filename
    result, external = {}, {}
    for page in root.rglob('*.html'):
        content = page.read_text(encoding='utf-8')
        for match in re.finditer(r'<img\b[^>]*>', content):
            image = _attributes(match[0])
            if (not image.get('src', '').endswith('.png')
                    or ('usemap' not in image and (image['src'] != page.with_suffix('.png').name
                                                  or not page.name.startswith(('class', 'struct'))))):
                continue
            map_id = image.get('usemap', '')[1:]
            image_map = (re.search(r'<map\b[^>]*\bid="' + re.escape(map_id) + r'"[^>]*>(.*?)</map>', content, re.S)
                         if map_id else None)
            if map_id and not image_map:
                raise ValueError(f'missing inheritance map {map_id} in {page}')
            current = compounds.get(page.name)
            if current is None:
                raise ValueError(f'missing Doxygen tag entry for inheritance diagram in {page}')
            nodes = {}
            for area in re.findall(r'<area\b[^>]*>', image_map[1] if image_map else ''):
                attributes = _attributes(area)
                href, label = attributes.get('href'), attributes.get('alt')
                if not href or not label:
                    raise ValueError(f'incomplete inheritance map node in {page}')
                filename = urlsplit(href).path
                if filename not in compounds:
                    raise ValueError(f'unresolved inheritance map target {href} in {page}')
                nodes[filename] = {'id': filename, 'label': label, 'href': href,
                                   'description': attributes.get('title', label)}
            caption = re.search(r'Inheritance diagram for (.*?):</div>', content, re.S)
            if not caption:
                raise ValueError(f'missing inheritance caption in {page}')
            title = html.unescape(re.sub(r'<[^>]*>', '', caption[1])).strip()
            nodes[page.name] = {'id': page.name, 'label': title, 'href': page.name,
                                'description': title, 'current': True}
            if not image_map and not current.findall('base'):
                external[page.name] = _external_bases(content, page, current.findtext('name'))
            def bases(filename):
                return [base.text or '' for base in compounds[filename].findall('base')] + external.get(filename, [])
            by_label = {''.join(node['label'].split()): key for key, node in nodes.items()}
            def resolve(label):
                return names.get(''.join(label.split())) or by_label.get(''.join(label.split()))
            # External bases have no clickable image-map area. The tag retains
            # their exact labels; include them along the current type's ancestry.
            visited = set()
            def ancestors(filename):
                if filename in visited:
                    return
                visited.add(filename)
                for label in bases(filename):
                    target = resolve(label)
                    if target and target in nodes:
                        ancestors(target)
                    elif target is None:
                        key = 'external:' + label
                        nodes[key] = {'id': key, 'label': label, 'description': label}
                        by_label[''.join(label.split())] = key
            ancestors(page.name)
            edges = []
            for filename in list(nodes):
                if filename not in compounds:
                    continue
                for label in bases(filename):
                    target = resolve(label)
                    if target in nodes:
                        edges.append({'from': filename, 'to': target})
            graph = {'title': title, 'current': page.name, 'nodes': list(nodes.values()), 'edges': edges}
            asset = (page.parent / image['src']).resolve()
            result[asset] = (graph, page.relative_to(root).as_posix())
    return result


def _inheritance_svg(graph, page, destination, palette):
    """Use vertical trunks to keep broad class hierarchies inside reading width."""
    esc = html.escape
    nodes = {node['id']: node for node in graph['nodes']}
    levels = {}
    def level(identity, active=frozenset()):
        if identity in active:
            raise ValueError(f'cyclic inheritance diagram: {graph["title"]}')
        if identity not in levels:
            parents = [edge['to'] for edge in graph['edges'] if edge['from'] == identity]
            levels[identity] = 1 + max((level(parent, active | {identity}) for parent in parents), default=-1)
        return levels[identity]
    ordered = sorted(nodes, key=lambda identity: (level(identity), nodes[identity]['label']))
    parent_ids = sorted({edge['to'] for edge in graph['edges']})
    gutter, character_width, card_padding, outer_margin = 72, 10, 32, 24
    wrapped = {identity: textwrap.wrap(nodes[identity]['label'], 37, break_long_words=True)
               for identity in ordered}
    card_widths = {identity: max(len(line) for line in wrapped[identity]) * character_width + card_padding
                   for identity in ordered}
    width = gutter + max(card_widths.values()) + outer_margin
    title_lines = textwrap.wrap(graph['title'], max(1, (width - 40) // character_width))
    y = 36 + 24 * len(title_lines)
    positions, records = {}, []
    for identity in ordered:
        node = nodes[identity]
        lines = wrapped[identity]
        height = 24 + 24 * len(lines)
        positions[identity] = (gutter, y, height)
        records.append((node, y, height, lines, card_widths[identity]))
        y += height + 24
    height = y
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
           f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
           f'<title id="title">{esc(graph["title"])}</title>',
           '<desc id="desc">Inheritance diagram. Arrows point to base types.</desc>',
           f'<metadata>{esc(json.dumps(graph, ensure_ascii=False))}</metadata>',
           f'<style>text{{font-family:{FONT};font-size:16px;fill:{palette["ink"]}}}'
           f'a:focus rect{{stroke:{palette["title"]};stroke-width:3}}</style>',
           '<defs><marker id="base" viewBox="0 0 10 10" refX="9" refY="5" '
           'markerWidth="10" markerHeight="10" orient="auto" markerUnits="userSpaceOnUse">'
           f'<path d="M0 0L10 5L0 10Z" fill="{palette["background"]}" stroke="{palette["accent"]}"/></marker></defs>',
           f'<rect width="{width}" height="{height}" fill="{palette["background"]}"/>']
    for index, line in enumerate(title_lines):
        out.append(f'<text x="20" y="{28 + index * 24}" fill="{palette["title"]}">{esc(line)}</text>')
    for edge in graph['edges']:
        x1, y1, h1 = positions[edge['from']]
        x2, y2, h2 = positions[edge['to']]
        lane = 16 + 5 * (parent_ids.index(edge['to']) % 9)
        route = f'M{x1} {y1 + h1 / 2}H{lane}V{y2 + h2 / 2}H{x2 - 2}'
        relation = nodes[edge['from']]['label'] + ' → ' + nodes[edge['to']]['label']
        out.append(f'<path data-from="{esc(edge["from"], quote=True)}" data-to="{esc(edge["to"], quote=True)}" '
                   f'd="{route}" fill="none" stroke="{palette["accent"]}" stroke-width="2" marker-end="url(#base)">'
                   f'<title>{esc(relation)}</title></path>')
    for node, y, height, lines, card_width in records:
        href = node.get('href')
        if href:
            target = posixpath.normpath(posixpath.join(posixpath.dirname(page), href))
            relative = posixpath.relpath(target, posixpath.dirname(destination))
            out.append(f'<a href="{esc(relative, quote=True)}" target="_top" aria-label="{esc(node["label"], quote=True)}">')
        else:
            out.append('<g>')
        out.append(f'<title>{esc(node["description"])}</title>')
        border = palette['title'] if node.get('current') else palette['border']
        out.append(f'<rect x="{gutter}" y="{y}" width="{card_width}" height="{height}" '
                   f'fill="{palette["panel"]}" stroke="{border}" stroke-width="2"/>')
        for index, line in enumerate(lines):
            out.append(f'<text x="{gutter + 16}" y="{y + 28 + index * 24}">{esc(line)}</text>')
        out.append('</a>' if href else '</g>')
    return '\n'.join(out + ['</svg>'])


def prepare_diagrams(root, source_root, tag_files=None):
    """Build variants for every diagram referenced by the generated site."""
    root, source_root = Path(root).resolve(), Path(source_root).resolve()
    cache = root.parent / '.theme-diagrams-cache'
    public = root / 'theme-diagrams'
    # Remove only this generator's old render bookkeeping from public output.
    for path in public.rglob('*') if public.exists() else ():
        if path.is_file() and path.suffix in {'.mmd', '.json', '.sha256'}:
            if not path.resolve().is_relative_to(public.resolve()):
                raise ValueError(f'diagram bookkeeping escapes its generated directory: {path}')
            path.unlink()
    catalog = DiagramCatalog(root)
    maps_path = source_root / "docs/architecture/snapshots/m1-968651a/maps.json"
    data = json.loads(maps_path.read_text(encoding="utf-8")) if maps_path.is_file() else None
    inheritance = _inheritance_graphs(root, tag_files)
    references = {}
    for page in root.rglob("*.html"):
        for reference in re.findall(r'(?:src|data)="([^"]+\.(?:svg|png|gif))"', page.read_text(encoding="utf-8")):
            url = urlsplit(html.unescape(reference))
            if url.scheme or url.netloc:
                continue
            if 'theme-diagrams' in Path(unquote(url.path)).parts:
                continue
            asset = (page.parent / unquote(url.path)).resolve()
            if asset.is_relative_to(root.resolve()) and asset.is_file():
                references.setdefault(asset, []).append(page.relative_to(root).as_posix())
    for asset, pages in sorted(references.items()):
        relative = asset.relative_to(root.resolve()).as_posix()
        kind, payload = None, None
        if asset in inheritance:
            kind, payload = 'inheritance', inheritance[asset]
        elif asset.name.startswith("inline_mermaid_") and asset.with_suffix(".mmd").is_file():
            kind, payload = "mermaid", asset.with_suffix(".mmd")
        elif asset.name in {"components.svg", "event-flow.svg", "ownership.svg"} and asset.parent in {root.resolve(), (root / "ru").resolve()}:
            payload = source_root / "docs/images/turbo" / asset.name
            if payload.is_file():
                kind = "architecture"
        elif data and asset.parent.name == data["id"]:
            for diagram in data["maps"]:
                for language in ("en", "ru"):
                    if asset.name == f'{diagram["id"]}-{language}.svg':
                        kind, payload = "snapshot", (diagram, language)
        if not kind:
            catalog.inventory.append({"asset": relative, "pages": pages, "kind": "unchanged"})
            continue
        variants = {}
        for theme, palette in PALETTES.items():
            destination = (root / "theme-diagrams" / theme / relative).with_suffix('.svg')
            destination.parent.mkdir(parents=True, exist_ok=True)
            source_bytes = (payload.read_bytes() if kind not in {"snapshot", "inheritance"}
                            else json.dumps([data if kind == "snapshot" else None, payload], ensure_ascii=False).encode())
            signature = hashlib.sha256(source_bytes + Path(__file__).read_bytes() + theme.encode()).hexdigest()
            stamp = (cache / theme / relative).with_suffix('.sha256')
            stamp.parent.mkdir(parents=True, exist_ok=True)
            if not (destination.is_file() and stamp.is_file() and stamp.read_text() == signature):
                if kind == "mermaid":
                    _render_mermaid(payload, destination, palette, stamp.parent)
                elif kind == "inheritance":
                    graph, page = payload
                    rendered = _inheritance_svg(graph, page, destination.relative_to(root).as_posix(), palette)
                    destination.write_text(rendered, encoding="utf-8")
                elif kind == "snapshot":
                    destination.write_text(_snapshot_svg(data, *payload, palette), encoding="utf-8")
                else:
                    rendered = payload.read_text(encoding="utf-8")
                    for color, replacement in zip(PALETTES["turbo"].values(), palette.values()):
                        rendered = rendered.replace(color, replacement)
                    destination.write_text(rendered, encoding="utf-8")
                stamp.write_text(signature, encoding="ascii")
            variants[theme] = destination.relative_to(root).as_posix()
        catalog.assets[relative] = variants
        record = {"asset": relative, "pages": pages, "kind": kind, "variants": variants}
        if kind == 'inheritance':
            record.update(nodes=len(payload[0]['nodes']), edges=len(payload[0]['edges']))
        catalog.inventory.append(record)
    inventory = cache / "inventory.json"
    inventory.parent.mkdir(parents=True, exist_ok=True)
    inventory.write_text(json.dumps(catalog.inventory, ensure_ascii=False, indent=2), encoding="utf-8")
    return catalog


def rewrite_diagrams(content, page, catalog):
    """Keep the original image and expose exactly one theme variant at a time."""
    page = Path(page)
    page = page.relative_to(catalog.root).as_posix() if page.is_absolute() else page.as_posix()
    def replace(match):
        original = match[0]
        if re.match(r'<span\b[^>]*\bclass="diagram-variants"', original):
            return original
        reference = re.search(r'\b(?:src|data)="([^"]+)"', original)
        if not reference or 'class="turbo-diagram"' in original or 'class="turbo2-diagram"' in original:
            return original
        url = urlsplit(html.unescape(reference[1]))
        if url.scheme or url.netloc:
            return original
        asset = posixpath.normpath(posixpath.join(posixpath.dirname(page), unquote(url.path)))
        variants = catalog.assets.get(asset)
        if not variants:
            return original
        # Existing named class/id/alt attributes stay on the native asset.
        if re.search(r'\bclass="', original):
            original = re.sub(r'\bclass="', 'class="standard-diagram ', original, count=1)
        else:
            original = original.replace(' ', ' class="standard-diagram" ', 1)
        alt = re.search(r'\b(?:alt|aria-label)="([^"]*)"', original)
        if alt and alt[1]:
            description = html.unescape(alt[1])
        else:
            svg = ET.parse(catalog.root / next(iter(variants.values()))).getroot()
            description = svg.findtext(SVG + "title") or Path(asset).stem
        rendered = [original]
        maximum_width = 0
        for theme, destination in variants.items():
            relative = posixpath.relpath(destination, posixpath.dirname(page) or ".")
            width, height = _dimensions((catalog.root / destination).read_text(encoding="utf-8"))
            maximum_width = max(maximum_width, width)
            rendered.append(f'<object class="{theme}-diagram" type="image/svg+xml" '
                            f'data="{html.escape(relative, quote=True)}" width="{width:g}" height="{height:g}" '
                            f'aria-label="{html.escape(description, quote=True)}">{html.escape(description)}</object>')
        return (f'<span class="diagram-variants" style="--turbo-diagram-width: {maximum_width:g}px">'
                + ''.join(rendered) + '</span>')
    return re.sub(r'<span\b[^>]*\bclass="diagram-variants"[^>]*>.*?</span>|<object\b[^>]*>.*?</object>|<img\b[^>]*>',
                  replace, content, flags=re.S)
