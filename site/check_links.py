#!/usr/bin/env python3
"""Crawl the site's local links and images; exit non-zero on a broken one.

External links (http/https) are not fetched: this checks the build, not the
internet. Anchors are resolved inside the target document.
"""
import pathlib
import re
import sys
import urllib.parse

root = pathlib.Path(__file__).resolve().parent
pages = sorted(root.glob("*.html"))
failures = []

for page in pages:
    text = page.read_text(encoding="utf-8")
    for attr in ("href", "src"):
        for match in re.finditer(attr + r'="([^"]+)"', text):
            target = match.group(1)
            if target.startswith(("http://", "https://", "mailto:", "data:")):
                continue
            if target.startswith("#"):
                continue
            split = urllib.parse.urlsplit(target)
            path, fragment = split.path, split.fragment
            if not path:
                continue
            resolved = (page.parent / urllib.parse.unquote(path)).resolve()
            if not resolved.exists():
                failures.append(f"{page.name}: {target} -> missing {resolved}")
                continue
            if fragment and resolved.suffix == ".html":
                body = resolved.read_text(encoding="utf-8")
                if f'id="{fragment}"' not in body:
                    failures.append(f"{page.name}: {target} -> no anchor #{fragment}")

for failure in failures:
    print(failure)
print(f"checked {len(pages)} page(s), {len(failures)} broken link(s)")
sys.exit(1 if failures else 0)
