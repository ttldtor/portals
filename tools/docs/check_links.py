# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
"""Validate local files and anchors in generated Doxygen HTML.

Reads the HTML tree without changing files. External URLs are not requested.
Writes a JSON summary to stdout; exits 0 on success, 1 on missing input/links
or read failure, and 2 on invalid CLI syntax. Invoke --help for usage.
"""
import argparse
import json
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlsplit, unquote

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("html", type=Path, help="Existing generated HTML directory; scanned recursively")
args = parser.parse_args()
if not args.html.is_dir():
    raise SystemExit("HTML directory does not exist or is not a directory: " + str(args.html))
root = args.html.resolve()
class Page(HTMLParser):
    """Collect local-link candidates and defined IDs from one HTML document."""
    def __init__(self):
        super().__init__()
        self.links = []
        self.ids = set()
    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs: self.ids.add(attrs["id"])
        if tag == "a" and "name" in attrs: self.ids.add(attrs["name"])
        for key in ("href", "src"):
            if key in attrs: self.links.append(attrs[key])

pages = {}
for file in root.rglob("*.html"):
    page = Page()
    page.feed(file.read_text(encoding="utf-8"))
    pages[file.resolve()] = page
if not pages:
    raise SystemExit('No HTML files found; generate documentation before checking links')

bad = []
checked = 0
for file, page in pages.items():
    for link in page.links:
        url = urlsplit(link)
        if url.scheme or url.netloc or link.startswith("javascript:"): continue
        target = (file.parent / unquote(url.path)).resolve() if url.path else file
        checked += 1
        if not target.exists(): bad.append([str(file.relative_to(root)), link, "missing file"])
        elif url.fragment and target in pages and unquote(url.fragment) not in pages[target].ids:
            bad.append([str(file.relative_to(root)), link, "missing anchor"])
print(json.dumps({"html_files":len(pages), "local_links_checked":checked, "broken":bad}, indent=2))
raise SystemExit(bool(bad))

