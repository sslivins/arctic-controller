#!/usr/bin/env python3
"""Check that the controller's fault troubleshooting links still work.

The controller links each fault code to an article on Arctic's Freshdesk
support site (main/fault_help_links.cpp). Arctic can delete or renumber those
articles at any time, and nothing on the device would notice, so this script
opens every link and checks that it still leads to the article for that code.

A link passes when the page:
  * answers 200 (Freshdesk answers 404 for a deleted article),
  * wasn't redirected to the sign-in page (an article made private), and
  * has a title that starts with the fault code, e.g. "P02 - High Pressure ...".
    Freshdesk looks articles up by number only, so a renumbered article would
    otherwise still "work" but show some other fault.

Usage: python tools/check_fault_help_links.py [--source PATH]
Exits 1 and lists the broken links if any fail.
"""

from __future__ import annotations

import argparse
import html
import re
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main" / "fault_help_links.cpp"

_ROW = re.compile(r'\{\s*"([A-Za-z0-9]+)"\s*,\s*"(https://[^"]+)"\s*\}')
_LIST = re.compile(r'SUPPORT_ARTICLE_LIST\s*=\s*"(https://[^"]+)"')
_TITLE = re.compile(r"<title[^>]*>(.*?)</title>", re.IGNORECASE | re.DOTALL)

USER_AGENT = "arctic-controller-link-check (+https://github.com/sslivins/arctic-controller)"
ATTEMPTS = 3


@dataclass(frozen=True)
class Link:
    code: str
    url: str


def parse_links(text: str) -> list[Link]:
    return [Link(code, url) for code, url in _ROW.findall(text)]


def parse_support_list_url(text: str) -> str | None:
    match = _LIST.search(text)
    return match.group(1) if match else None


def page_title(body: str) -> str:
    match = _TITLE.search(body)
    return " ".join(html.unescape(match.group(1)).split()) if match else ""


def check_page(code: str | None, status: int, final_url: str, body: str) -> str | None:
    """Return why the page is wrong, or None when it's the right article."""
    if status != 200:
        return f"HTTP {status}"
    if "/support/login" in final_url:
        return f"redirected to the sign-in page ({final_url})"
    if code is None:
        return None
    title = page_title(body)
    if not re.match(rf"{re.escape(code)}\b", title, re.IGNORECASE):
        return f"page title {title!r} isn't about {code}"
    return None


def fetch(url: str, timeout: float = 20.0) -> tuple[int, str, str]:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read().decode("utf-8", errors="replace")
            return response.status, response.geturl(), body
    except urllib.error.HTTPError as err:
        return err.code, err.geturl() or url, ""


def check_url(code: str | None, url: str) -> str | None:
    problem = None
    for attempt in range(ATTEMPTS):
        try:
            status, final_url, body = fetch(url)
        except (urllib.error.URLError, TimeoutError, OSError) as err:
            problem = f"couldn't connect ({err})"
        else:
            problem = check_page(code, status, final_url, body)
            # A missing article is a definite answer; only retry the unclear ones.
            if problem is None or status == 404:
                return problem
        if attempt + 1 < ATTEMPTS:
            time.sleep(5 * (attempt + 1))
    return problem


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--source", type=Path, default=SOURCE)
    args = parser.parse_args(argv)

    text = args.source.read_text(encoding="utf-8")
    links = parse_links(text)
    if not links:
        print(f"No links found in {args.source}", file=sys.stderr)
        return 1

    checks: list[tuple[str | None, str]] = [(link.code, link.url) for link in links]
    support_list = parse_support_list_url(text)
    if support_list:
        checks.append((None, support_list))

    broken = []
    for code, url in checks:
        problem = check_url(code, url)
        label = code or "support site"
        print(f"{'FAIL' if problem else 'ok  '} {label:<12} {url}" + (f"  -> {problem}" if problem else ""))
        if problem:
            broken.append(f"- **{label}** {url}: {problem}")

    if broken:
        print(f"\n{len(broken)} of {len(checks)} troubleshooting links are broken:")
        print("\n".join(broken))
        return 1
    print(f"\nAll {len(checks)} troubleshooting links work.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
