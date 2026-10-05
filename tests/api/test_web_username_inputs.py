"""Static contract: phone keyboards must not alter typed usernames.

Usernames are case-sensitive, and mobile keyboards capitalize the first
letter by default, so signing in as "arctic" from a phone typed "Arctic".
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_every_username_input_disables_autocapitalize_and_autocorrect() -> None:
    index_html = (ROOT / "main" / "web" / "index.html").read_text(encoding="utf-8")
    inputs = re.findall(r'<input name="username"[^>]*>', index_html)

    assert inputs, "no username inputs found"
    for tag in inputs:
        assert 'autocapitalize="none"' in tag, tag
        assert 'autocorrect="off"' in tag, tag
        assert 'spellcheck="false"' in tag, tag
