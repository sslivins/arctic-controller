"""The fault troubleshooting link table and its link checker.

The live check (tools/check_fault_help_links.py, run weekly by
.github/workflows/fault-help-links.yml) needs the internet; these tests only
cover the table and the checker's own logic, so they run offline.
"""

import importlib.util
import re
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).parents[1]
_spec = importlib.util.spec_from_file_location(
    "check_fault_help_links", ROOT / "tools" / "check_fault_help_links.py")
links_tool = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = links_tool  # dataclasses look their module up here
_spec.loader.exec_module(links_tool)

SOURCE = (ROOT / "main" / "fault_help_links.cpp").read_text(encoding="utf-8")
ARTICLE_URL = re.compile(r"^https://arcticheatpumps\.freshdesk\.com/support/solutions/articles/\d+$")


def test_the_checker_finds_every_row_of_the_table():
    rows = re.findall(r"^\s*\{\"", SOURCE, re.MULTILINE)
    links = links_tool.parse_links(SOURCE)
    assert len(links) == len(rows) >= 20


def test_each_code_has_one_article_link():
    links = links_tool.parse_links(SOURCE)
    codes = [link.code.upper() for link in links]
    assert len(codes) == len(set(codes)), "a code is listed twice"
    for link in links:
        # No title slug: it isn't needed, and a shorter URL makes a smaller QR code.
        assert ARTICLE_URL.match(link.url), link


def test_codes_without_an_article_fall_back_to_the_support_site():
    assert links_tool.parse_support_list_url(SOURCE) == \
        "https://arcticheatpumps.freshdesk.com/support/solutions"


ARTICLE = "<html><head><title>P02 - High Pressure Protection : Arctic Heat Pumps</title></head></html>"


@pytest.mark.parametrize("code,status,url,body,ok", [
    ("P02", 200, "https://x/support/solutions/articles/1", ARTICLE, True),
    ("p02", 200, "https://x/support/solutions/articles/1", ARTICLE, True),
    ("P02", 404, "https://x/support/solutions/articles/1", "", False),
    ("P02", 200, "https://x/support/login", "<title>Login</title>", False),
    # Renumbered: the link still opens, but on a different fault.
    ("P06", 200, "https://x/support/solutions/articles/1", ARTICLE, False),
    ("P0", 200, "https://x/support/solutions/articles/1", ARTICLE, False),
    (None, 200, "https://x/support/solutions", "<title>Solutions</title>", True),
])
def test_check_page(code, status, url, body, ok):
    assert (links_tool.check_page(code, status, url, body) is None) == ok
