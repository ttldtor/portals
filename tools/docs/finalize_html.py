"""Finalize Doxygen HTML: API links, CMake highlighting, navigation, search and language.

Run automatically through the portals_docs target. For diagnosis, pass the
existing HTML directory and Doxygen XML tag file from the same build. This
rewrites guide HTML and navigation/search JS in place and copies linked repository
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
from urllib.parse import urlsplit, urlunsplit


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


def portable_links(content, directory, source, links, russian=False, defer_markdown=False):
    """Resolve checkout-relative links and copy source files within the repository."""
    def asset_link(match):
        href = html.unescape(match[2])
        if re.match(r"[a-z]+:", href) or href.startswith(("#", "/")):
            return match[0]
        asset = urlsplit(href)
        # Imported API and language links may intentionally point to the parent site.
        if asset.path.endswith(".html") and (directory / asset.path).is_file():
            return match[0]
        flat_path = re.sub(r"^(?:\.\./)+", "", asset.path)
        destination = (directory / flat_path).resolve()
        if destination.is_relative_to(directory.resolve()) and destination.is_file():
            url = urlunsplit(("", "", flat_path, asset.query, asset.fragment))
            return match[1] + html.escape(url, quote=True) + '"'
        return match[0]

    content = re.sub(r'\b((?:href|src)=")([^"]+)"', asset_link, content)

    def rewrite(match):
        href, label = html.unescape(match[1]), match[2]
        if re.match(r"[a-z]+:", href) or href.startswith(("#", "/")):
            return match[0]
        destination = (directory / urlsplit(href).path).resolve()
        if destination.is_relative_to(directory.resolve()) and destination.is_file():
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
            if defer_markdown:
                return match[0]
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


def document_language(content, language):
    """Set the page language on the root element without changing code or links."""
    def root(match):
        tag = match[0]
        for attribute in ("lang", "xml:lang"):
            pattern = rf'\s{attribute}=["\'][^"\']*["\']'
            value = f' {attribute}="{language}"'
            if re.search(pattern, tag):
                tag = re.sub(pattern, lambda _: value, tag)
            else:
                tag = tag[:-1] + value + ">"
        return tag

    return re.sub(r"<html\b[^>]*>", root, content, count=1)


def complete_search(directory):
    """Initialize shared local search when generated HTML lacks Doxygen config."""
    script = directory / "search" / "search.js"
    if not script.is_file():
        return
    text = script.read_text(encoding="utf-8")
    marker = "// Portals search initialization for layouts without doxygen-config."
    config_gate = "const configElem = document.getElementById('doxygen-config');"
    if marker in text or config_gate not in text:
        return
    text += "\n" + marker + "\n" + """document.addEventListener('DOMContentLoaded', () => {
  if (!document.getElementById('doxygen-config') &&
      document.getElementById('MSearchField') &&
      typeof searchBox !== 'undefined') {
    init_search();
  }
});
"""
    script.write_text(text, encoding="utf-8")


def finalize(directory, tag_file, language=None, api_tag=None, api_prefix="../"):
    """Rewrite registered guide pages and their navigation after Doxygen finishes."""
    links = type_links(api_tag or tag_file)
    if api_tag:
        links = {name: api_prefix + target for name, target in links.items()}
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
        russian = language == "ru" if language else name.endswith("-ru.html") or "_2ru_2" in name
        content = portable_links("".join(parser.output), directory,
                                 Path(__file__).resolve().parents[2], links,
                                 russian, defer_markdown=language is not None)
        content = document_language(content, language or ("ru" if russian else "en"))
        path.write_text(color_cmake(content), encoding="utf-8")
        headings[name] = parser.headings
    complete_navigation(directory, headings)
    complete_search(directory)
    print(json.dumps({"guide_pages": len(headings), "unambiguous_types": len(links)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("html", type=Path, help="Existing generated HTML directory, modified in place")
    parser.add_argument("tag_file", type=Path, help="Doxygen XML tag file from the same documentation build")
    parser.add_argument("--language-sites", nargs="*", default=None, metavar="CODE=TAG",
                        help="Finalize separate language sites from docs/languages.json after the English build; each value supplies its locale tag file")
    args = parser.parse_args()
    if args.language_sites is None:
        finalize(args.html, args.tag_file)
    else:
        from language_site import finalize_languages
        source = Path(__file__).resolve().parents[2]
        config = json.loads((source / "docs/languages.json").read_text(encoding="utf-8"))
        tags = {config["default"]: args.tag_file}
        for value in args.language_sites:
            code, separator, filename = value.partition("=")
            if not separator or code in tags:
                parser.error("Expected a unique CODE=TAG for each additional language")
            tags[code] = Path(filename)
        if set(tags) != {locale["code"] for locale in config["languages"]}:
            parser.error("Supply exactly one CODE=TAG for every additional configured language")
        for locale in config["languages"]:
            code = locale["code"]
            finalize(args.html / locale["directory"], tags[code], code,
                     args.tag_file if code != config["default"] else None,
                     "../" * len(Path(locale["directory"]).parts))
        finalize_languages(args.html, source, tags)
