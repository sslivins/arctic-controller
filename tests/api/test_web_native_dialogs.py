"""Static contract: the web UI uses its themed dialog, never browser popups.

Native confirm()/prompt()/alert() boxes ignore the theme and, on phones,
show the page address as their title.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_web_ui_has_no_native_browser_dialogs() -> None:
    index_html = (ROOT / "main" / "web" / "index.html").read_text(encoding="utf-8")
    calls = re.findall(r"(?<![\w.])(?:window\.)?(?:confirm|prompt|alert)\(", index_html)

    assert not calls, f"use askConfirm() instead of native dialogs: {calls}"
    assert "function askConfirm(" in index_html
