"""Internal multilingual-site finalizer, called by finalize_html.py.

Reads docs/languages.json, generated tag files and HTML after each language's
normal finalization. Rewrites inter-language links, adds page-level language
navigation and writes redirects for former translated URLs. All changes stay in
the generated site; I/O, invalid configuration and missing main pages raise errors.
"""

import html
import json
import posixpath
from pathlib import Path
import re
import xml.etree.ElementTree as ET
from urllib.parse import unquote, urlsplit, urlunsplit


def page_filename(source, relative):
    """Return the existing Doxygen filename convention for a Markdown source."""
    first = (source / relative).read_text(encoding="utf-8").splitlines()[0]
    explicit = re.search(r"\{#([^}]+)\}", first)
    return (explicit[1] if explicit else "md_" + relative[:-3].replace("/", "_2")) + ".html"


def pages_for(source, locale, tag_file, excluded=()):
    """Match only generated pages; an untranslated source never becomes a page."""
    compounds = [c for c in ET.parse(tag_file).getroot().findall("compound")
                 if c.get("kind") == "page"]
    generated = {c.findtext("filename") for c in compounds}
    pages = {}
    for path in (source / locale["source"]).rglob("*.md"):
        relative = path.relative_to(source).as_posix()
        # The default source directory contains translation directories as well.
        if any(path.is_relative_to(source / prefix) for prefix in excluded):
            continue
        legacy = page_filename(source, relative)
        filename = "index.html" if relative == locale["main"] else legacy
        if filename not in generated:
            continue
        topic = path.relative_to(source / locale["source"]).as_posix()
        pages[topic] = {"source": relative, "filename": filename, "legacy": legacy}
    return pages


def additional_pages(source, tag_file, known_sources):
    """Keep generated repository pages such as vendor notices reachable from translations."""
    pages = {}
    for compound in ET.parse(tag_file).getroot().findall("compound"):
        if compound.get("kind") not in {"page", "dir"}:
            continue
        name = compound.findtext("name", "")
        candidates = [anchor.get("title", "") for anchor in compound.findall("docanchor")]
        if name.startswith("md_"):
            candidates.append(name[3:].replace("_2", "/") + ".md")
            candidates.append(re.sub(r"_([a-z])", lambda match: match[1].upper(),
                                     name[3:].replace("_2", "/")) + ".md")
        if compound.get("kind") == "dir":
            candidates.append(compound.findtext("path", "") + "README.md")
        for relative in candidates:
            path = source / relative
            if (relative.endswith(".md") and relative not in known_sources and path.is_file()
                    and path.resolve().is_relative_to(source.resolve())):
                filename = compound.findtext("filename")
                pages["@" + relative] = {"source": relative, "filename": filename, "legacy": filename}
                break
    return pages


def relative_url(current, target):
    return posixpath.relpath(target, posixpath.dirname(current) or ".")


def normalized_anchor(anchor, code):
    if anchor.startswith("autotoc_md"):
        return None
    anchor = anchor.replace(f"md_docs_2{code}_2", "md_docs_2")
    return anchor.removesuffix("-" + code) if code != "en" else anchor


def anchor_map(current, target, source_code, target_code):
    """Pair explicit IDs only; generated heading numbers have no stable identity."""
    def ids(content):
        return re.findall(r'\b(?:id|name)="([^"]+)"', content)

    targets = {}
    for value in ids(target):
        key = normalized_anchor(value, target_code)
        if key:
            targets.setdefault(key, []).append(value)
    return {value: targets[key][0] for value in ids(current)
            if (key := normalized_anchor(value, source_code)) and len(targets.get(key, [])) == 1}


SWITCH_SCRIPT = """// Preserve a section only when the build verified its translated ID.
document.addEventListener('DOMContentLoaded', () => {
  const update = () => {
    let anchor;
    try { anchor = decodeURIComponent(location.hash.slice(1)); }
    catch { anchor = ''; }
    document.querySelectorAll('a[data-language-page]').forEach(link => {
      const anchors = JSON.parse(link.dataset.languageAnchors);
      link.href = link.dataset.languagePage + (anchors[anchor] ? '#' + anchors[anchor] : '');
    });
  };
  update();
  window.addEventListener('hashchange', update);
});
"""


def redirect(path, target):
    """Keep fragment/query on old translated URLs, with a plain-link fallback."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('<!doctype html><html lang="en"><head><meta charset="utf-8">'
                    '<title>Documentation moved</title><script>location.replace('
                    + json.dumps(target) + '+location.search+location.hash);</script>'
                    '<meta http-equiv="refresh" content="0;url=' + html.escape(target, quote=True)
                    + '"></head><body><a href="' + html.escape(target, quote=True)
                    + '">Continue / Продолжить</a></body></html>', encoding="utf-8")


def finalize_languages(root, source, tag_files):
    """Assemble language navigation after all locale builds have completed."""
    config = json.loads((source / "docs/languages.json").read_text(encoding="utf-8"))
    locales = {item["code"]: item for item in config["languages"]}
    if set(locales) != set(tag_files):
        raise ValueError("Every configured language requires its generated tag file")
    default = config["default"]
    pages = {code: pages_for(source, locale, tag_files[code],
                            [other["source"] for other in locales.values()
                             if other["source"].startswith(locale["source"] + "/")])
             for code, locale in locales.items()}
    known = {page["source"] for topics in pages.values() for page in topics.values()}
    pages[default].update(additional_pages(source, tag_files[default], known))
    by_source, by_url, aliases = {}, {}, {}
    for code, topics in pages.items():
        for topic, page in topics.items():
            url = posixpath.join(locales[code]["directory"], page["filename"])
            page.update(url=url, code=code, topic=topic)
            by_source[page["source"]] = page
            by_url[url] = page
            if code != default:
                aliases[page["legacy"]] = page
    contents = {url: (root / url).read_text(encoding="utf-8") for url in by_url}
    for code, locale in locales.items():
        directory = root / locale["directory"]
        (directory / "language-switch.js").write_text(SWITCH_SCRIPT, encoding="utf-8")
        for path in directory.glob("*.html"):
            current = path.relative_to(root).as_posix()
            page = by_url.get(current)
            content = path.read_text(encoding="utf-8")
            content = re.sub(r'<nav class="portals-languages".*?</nav>', '', content, flags=re.S)
            content = content.replace('<script src="language-switch.js" defer></script>\n', '')

            def link(match):
                raw = html.unescape(match[2])
                url = urlsplit(raw)
                if url.scheme or url.netloc or raw.startswith(("#", "/")):
                    return match[0]
                target = posixpath.normpath(posixpath.join(locale["directory"], unquote(url.path)))
                candidate = by_url.get(target) or aliases.get(url.path)
                if url.path.endswith(".md"):
                    base = posixpath.dirname(page["source"]) if page else locale["source"]
                    candidate = by_source.get(posixpath.normpath(posixpath.join(base, url.path)))
                if candidate is None:
                    return match[0]
                resolved = relative_url(current, candidate["url"])
                return match[1] + html.escape(urlunsplit(("", "", resolved, url.query, url.fragment)), quote=True) + '"'

            content = re.sub(r'(\bhref=")([^"]+)"', link, content)
            if code != default:
                def english_label(match):
                    target = urlsplit(html.unescape(match[2]))
                    if target.scheme or target.netloc:
                        return match[0]
                    url = posixpath.normpath(posixpath.join(locale["directory"], target.path))
                    destination = by_url.get(url)
                    label = html.unescape(re.sub(r'<[^>]*>', '', match[3]))
                    marked = re.search(r'\b(?:English|EN|английск\w*)\b', label, flags=re.I)
                    if not destination or destination["code"] != default or marked:
                        return match[0]
                    return match[1] + match[3] + ' <span lang="en">(English)</span></a>'
                content = re.sub(r'(<a\b[^>]*href="([^"]+)"[^>]*>)(.*?)</a>', english_label,
                                 content, flags=re.S)
            label = "Язык" if code == "ru" else "Language"
            controls = [f'<nav class="portals-languages" aria-label="{label}">']
            for other, alternative in locales.items():
                name = html.escape(alternative["name"])
                counterpart = pages[other].get(page["topic"]) if page else None
                if other == code:
                    controls.append(f'<span lang="{other}" aria-current="page">{name}</span>')
                elif counterpart:
                    target = relative_url(current, counterpart["url"])
                    mapping = anchor_map(contents[current], contents[counterpart["url"]], code, other)
                    controls.append(f'<a lang="{other}" hreflang="{other}" href="{target}" '
                                    f'data-language-page="{target}" data-language-anchors="'
                                    + html.escape(json.dumps(mapping), quote=True) + f'">{name}</a>')
                else:
                    missing = "перевода нет" if code == "ru" else "translation unavailable"
                    controls.append(f'<span lang="{other}">{name} ({missing})</span>')
            controls.append('</nav>')
            # The native links work without JavaScript; script only adds mapped hashes.
            content = re.sub(r'(<div id="titlearea">)', r'\1' + "".join(controls), content, count=1)
            content = content.replace('</head>', '<script src="language-switch.js" defer></script>\n</head>', 1)
            path.write_text(content, encoding="utf-8")
    for legacy, page in aliases.items():
        if legacy in by_url:
            raise ValueError("Translated legacy URL conflicts with an English page: " + legacy)
        redirect(root / legacy, page["url"])
    print(json.dumps({"language_pages": {code: len(topics) for code, topics in pages.items()},
                      "legacy_redirects": len(aliases)}))
