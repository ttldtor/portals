"""Finalize Doxygen HTML: API links, CMake highlighting and navigation.

Run automatically through the portals_docs target. For diagnosis, pass the
existing HTML directory and Doxygen XML tag file from the same build. This
rewrites guide HTML and navigation JS in place and copies linked repository
sources into HTML/source. Keep the script in its checkout. No network access.
Prints JSON guide/type counts; exits 0 on success, 1 on an uncaught I/O/XML/HTML
processing failure, 2 on invalid CLI syntax. Rebuild portals_docs after failure.
"""

import argparse
import html
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as ET


def type_links(tag_file):
    """Map only unambiguous documented type names to their generated API pages."""
    candidates = {}
    for compound in ET.parse(tag_file).getroot().findall("compound"):
        if compound.get("kind") not in {"class", "struct", "concept"}:
            continue
        name, filename = compound.findtext("name"), compound.findtext("filename")
        if not name or not filename:
            continue
        for key in {name, name.removeprefix("portals::"), name.rsplit("::", 1)[-1]}:
            candidates.setdefault(key, set()).add(filename)
    return {key: next(iter(values)) for key, values in candidates.items() if len(values) == 1}


class GuideParser(HTMLParser):
    """Link inline type references and collect headings without nesting anchors."""
    def __init__(self, links):
        super().__init__(convert_charrefs=False)
        self.links = links
        self.output = []
        self.anchor_depth = 0
        self.pre_depth = 0
        self.capture = None
        self.heading = None
        self.headings = []

    def emit(self, text):
        (self.capture["parts"] if self.capture else self.output).append(text)

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        raw = self.get_starttag_text()
        inline = tag == "code" or (tag == "span" and attributes.get("class") == "tt")
        if inline and not self.capture and not self.anchor_depth and not self.pre_depth:
            self.capture = {"tag": tag, "open": raw, "parts": []}
        else:
            self.emit(raw)
        if tag == "a":
            self.anchor_depth += 1
            if self.heading and attributes.get("id"):
                self.heading["id"] = attributes["id"]
        if tag == "pre":
            self.pre_depth += 1
        if tag in {"h1", "h2", "h3"}:
            self.heading = {"level": int(tag[1]), "id": attributes.get("id"), "text": []}

    def handle_endtag(self, tag):
        if self.capture and tag == self.capture["tag"]:
            value = "".join(self.capture["parts"])
            target = self.links.get(html.unescape(value).split("<", 1)[0].strip())
            rendered = self.capture["open"] + value + f"</{tag}>"
            if target and "<" not in value:
                rendered = f'<a class="el" href="{target}">{rendered}</a>'
            self.capture = None
            self.output.append(rendered)
        else:
            self.emit(f"</{tag}>")
        if tag == "a":
            self.anchor_depth -= 1
        if tag == "pre":
            self.pre_depth -= 1
        if self.heading and tag == f'h{self.heading["level"]}':
            if self.heading["id"]:
                self.headings.append((self.heading["level"], self.heading["id"],
                                      "".join(self.heading["text"]).strip()))
            self.heading = None

    def handle_data(self, data):
        self.emit(data)
        if self.heading:
            self.heading["text"].append(data)

    def handle_entityref(self, name):
        self.emit(f"&{name};")
        if self.heading:
            self.heading["text"].append(html.unescape(f"&{name};"))

    def handle_charref(self, name):
        self.emit(f"&#{name};")

    def handle_comment(self, data):
        self.emit(f"<!--{data}-->")

    def handle_decl(self, decl):
        self.emit(f"<!{decl}>")

    def handle_startendtag(self, tag, attrs):
        self.emit(self.get_starttag_text())


def color_cmake(content):
    """Color recognized plain CMake fragments with Doxygen CSS token classes.

    This is a small line-oriented lexer, not a complete CMake grammar. Existing
    highlighted fragments remain intact. Bracket strings/comments are not parsed.
    """
    def fragment(match):
        block = match[0]
        if "cmake-colors" in block or re.search(r'<span class="(?:keyword|preprocessor)', block):
            return block
        plain = html.unescape(re.sub(r"<[^>]+>", "", block))
        if not re.search(r"\b(?:cmake_minimum_required|project|find_package|FetchContent_Declare|target_link_libraries|add_subdirectory)\s*\(", plain):
            return block

        def line(match):
            value = html.unescape(match[1])
            tokens = re.compile(r'#[^\n]*|"(?:\\.|[^"\\])*"|\$\{[^}]+\}|\b[A-Za-z_][A-Za-z_0-9]*(?=\s*\()')
            parts, previous = [], 0
            for token in tokens.finditer(value):
                parts.append(html.escape(value[previous:token.start()]))
                text = token[0]
                kind = "comment" if text.startswith("#") else "stringliteral" if text.startswith('"') else "keyword"
                parts.append(f'<span class="{kind}">{html.escape(text)}</span>')
                previous = token.end()
            parts.append(html.escape(value[previous:]))
            return '<div class="line">' + "".join(parts) + "</div>"

        block = re.sub(r'<div class="line">([^<]*)</div>', line, block)
        return block.replace('class="fragment"', 'class="fragment cmake-colors"', 1)

    return re.sub(r'<div class="fragment">.*?</div>\s*</div>', fragment, content, flags=re.S)


def read_variable(text, name):
    match = re.search(rf"\bvar {name}\s*=\s*", text)
    if not match:
        raise ValueError(f"Missing Doxygen variable {name}")
    value, length = json.JSONDecoder().raw_decode(text[match.end():])
    return value, (match.end(), match.end() + length)


def write_variable(text, name, value):
    _, (start, end) = read_variable(text, name)
    return text[:start] + json.dumps(value, ensure_ascii=True, indent=2) + text[end:]


def complete_navigation(directory, headings):
    """Attach guide headings to NAVTREE and rebuild matching index paths."""
    tree_file = directory / "navtreedata.js"
    text = tree_file.read_text(encoding="utf-8")
    tree, _ = read_variable(text, "NAVTREE")
    additions = {}
    expanded = set()

    def visit(nodes, path):
        for index, node in enumerate(nodes):
            position = [*path, index]
            url = node[1] or ""
            filename = url.split("#")[0]
            if filename in headings:
                additions.setdefault(filename, position)
                additions.setdefault(url, position)
                first_visit = filename not in expanded
                expanded.add(filename)
                if node[2] is None and first_visit:
                    children = []
                    for _, anchor, title in headings[filename]:
                        children.append([title, filename + "#" + anchor, None])
                    node[2] = children or None
            if isinstance(node[2], list):
                visit(node[2], position)

    # Doxygen's index paths omit the outer project node (see navtree.js).
    visit(tree[0][2], [])
    indexes = {}
    old_files = sorted(directory.glob("navtreeindex[0-9]*.js"))
    for path in old_files:
        value, _ = read_variable(path.read_text(encoding="utf-8"), path.stem.upper())
        indexes.update(value)
    indexes.update(additions)
    keys = sorted(indexes)
    boundaries = []
    for number, start in enumerate(range(0, len(keys), 250)):
        chunk = keys[start:start + 250]
        boundaries.append(chunk[0])
        (directory / f"navtreeindex{number}.js").write_text(
            f"var NAVTREEINDEX{number} =\n" + json.dumps({key: indexes[key] for key in chunk}, indent=2) + ";\n",
            encoding="utf-8")
    text = write_variable(text, "NAVTREE", tree)
    tree_file.write_text(write_variable(text, "NAVTREEINDEX", boundaries), encoding="utf-8")


def portable_links(content, directory, source, links, russian=False):
    """Resolve checkout-relative links and copy source files within the repository."""
    def rewrite(match):
        href, label = html.unescape(match[1]), match[2]
        if re.match(r"[a-z]+:", href) or href.startswith(("#", "/")):
            return match[0]
        target, separator, anchor = href.partition("#")
        if target.endswith(".html"):
            return match[0]
        normalized = re.sub(r"^(?:\.\./)+", "", target)
        base = source / "docs" / "ru" if russian else source / "docs"
        candidates = [base / target, source / normalized, source / "docs" / normalized]
        file = next((p.resolve() for p in candidates if p.is_file()), None)
        if file is None or not file.is_relative_to(source):
            return match[0]
        if file.suffix == ".md":
            first = file.read_text(encoding="utf-8").splitlines()[0]
            explicit = re.search(r"\{#([^}]+)\}", first)
            relative = file.relative_to(source).as_posix().removesuffix(".md")
            filename = (explicit[1] if explicit else "md_" + relative.replace("/", "_2")) + ".html"
            if relative == "docs/index":
                filename = "index.html"
            if not (directory / filename).is_file():
                return match[0]
            url = filename + ("#" + anchor if separator else "")
        else:
            name = html.unescape(re.sub(r"<[^>]*>", "", label)).strip()
            url = links.get(name)
            if url is None:
                relative = file.relative_to(source)
                destination = directory / "source" / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(file, destination)
                url = "source/" + relative.as_posix() + ("#" + anchor if separator else "")
        return '<a href="' + html.escape(url, quote=True) + '">' + label + "</a>"

    return re.sub(r'<a\b[^>]*href="([^"]+)"[^>]*>(.*?)</a>', rewrite, content, flags=re.S)


def finalize(directory, tag_file):
    """Rewrite registered guide pages and their navigation after Doxygen finishes."""
    links = type_links(tag_file)
    pages = {"index.html"}
    for compound in ET.parse(tag_file).getroot().findall("compound"):
        if compound.get("kind") == "page":
            pages.add(compound.findtext("filename"))
    headings = {}
    for name in sorted(pages - {None}):
        path = directory / name
        if not path.is_file():
            continue
        parser = GuideParser(links)
        parser.feed(path.read_text(encoding="utf-8"))
        content = portable_links("".join(parser.output), directory,
                                 Path(__file__).resolve().parents[2], links,
                                 name.endswith("-ru.html") or "_2ru_2" in name)
        path.write_text(color_cmake(content), encoding="utf-8")
        headings[name] = parser.headings
    complete_navigation(directory, headings)
    print(json.dumps({"guide_pages": len(headings), "unambiguous_types": len(links)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("html", type=Path, help="Existing generated HTML directory, modified in place")
    parser.add_argument("tag_file", type=Path, help="Doxygen XML tag file from the same documentation build")
    args = parser.parse_args()
    finalize(args.html, args.tag_file)
